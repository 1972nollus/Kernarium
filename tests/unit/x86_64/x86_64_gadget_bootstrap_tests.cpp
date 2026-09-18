/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/cpuid.hpp"
#include "cpu/x86_64/decoder.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
#include "cpu/x86_64/gadgets/native_baked.hpp"
#include "cpu/x86_64/gadgets/native_entry.hpp"
#include "cpu/x86_64/gadgets/native_symbols.hpp"
#include "cpu/x86_64/gadgets/program.hpp"
#include "memory/guest_address_space.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

class TestMemory final : public uml::memory::GuestAddressSpace {
public:
	/* Most tests need a few pages. The repeat-budget tests need enough to
	 * run past a whole executor entry's worth of copying, which is far more
	 * than is worth giving every other test, so the span is a parameter and
	 * the storage is on the heap.
	 */
	explicit TestMemory(std::uint64_t base = 0, std::size_t span = 16384)
		: base_{base}, bytes_(span),
		  code_stamp_versions_(span / 4096U + 1U, 1) {}

	void load(std::uint64_t address,
		std::initializer_list<std::uint8_t> bytes) noexcept
	{
		const auto first = address;
		for (const auto byte : bytes)
			bytes_[address++ - base_] = static_cast<std::byte>(byte);
		bump_code_stamps(first, bytes.size());
	}

	void fill(std::uint64_t address,
		std::span<const std::byte> source) noexcept
	{
		std::memcpy(bytes_.data() + (address - base_), source.data(),
			source.size());
		bump_code_stamps(address, source.size());
	}

	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		if (address < base_ || address - base_ > bytes_.size() ||
			destination.size() > bytes_.size() - (address - base_)) {
			fault_ = {address, uml::memory::MemoryAccess::read};
			faulted_ = true;
			return false;
		}
		std::memcpy(destination.data(), bytes_.data() + (address - base_),
			destination.size());
		return true;
	}

	[[nodiscard]] bool fetch(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		++fetches_;
		if (!read(address, destination)) {
			fault_.access = uml::memory::MemoryAccess::execute;
			return false;
		}
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> source) override
	{
		if (address < base_ || address - base_ > bytes_.size() ||
			source.size() > bytes_.size() - (address - base_)) {
			fault_ = {address, uml::memory::MemoryAccess::write};
			faulted_ = true;
			return false;
		}
		std::memcpy(bytes_.data() + (address - base_),
			source.data(), source.size());
		bump_code_stamps(address, source.size());
		return true;
	}

	[[nodiscard]] std::byte *atomic_pointer(std::uint64_t address,
		std::size_t size, uml::memory::MemoryAccess access) override
	{
		atomic_pointers_.fetch_add(1, std::memory_order_relaxed);
		if (!atomic_pointer_available_)
			return nullptr;
		if (address < base_ || address - base_ > bytes_.size() ||
			size > bytes_.size() - (address - base_)) {
			fault_ = {address, access};
			faulted_ = true;
			return nullptr;
		}
		return bytes_.data() + (address - base_);
	}

	[[nodiscard]] bool is_executable(std::uint64_t address) noexcept override
	{
		executable_queries_.fetch_add(1, std::memory_order_relaxed);
		return every_page_executable_ ||
			(address & ~std::uint64_t{4095}) == executable_page_;
	}

	[[nodiscard]] bool code_page_stamp(std::uint64_t address, void *,
		uml::memory::CodePageStamp &stamp) noexcept override
	{
		++code_stamp_queries_;
		if (!code_stamps_available_ || address < base_)
			return false;
		const auto page = static_cast<std::size_t>((address - base_) / 4096U);
		if (page >= code_stamp_versions_.size())
			return false;
		stamp = {page + 1U, code_stamp_versions_[page]};
		return true;
	}

	void invalidate_code(std::uint64_t) override
	{
		++invalidations_;
		if (code_changes_ != nullptr)
			++*code_changes_;
	}
	void set_executable_page(std::uint64_t address) noexcept
	{
		executable_page_ = address & ~std::uint64_t{4095};
	}
	/* What READ_IMPLIES_EXEC makes of a process: data pages that qualify as
	 * code too. */
	void set_every_page_executable(bool value) noexcept
	{
		every_page_executable_ = value;
	}

	[[nodiscard]] uml::memory::MemoryFault last_fault() const noexcept override
	{
		return fault_;
	}

	[[nodiscard]] bool has_fault() const noexcept override { return faulted_; }

	[[nodiscard]] std::uint64_t qword(std::uint64_t address) const noexcept
	{
		std::uint64_t value{};
		std::memcpy(&value, bytes_.data() + (address - base_), sizeof(value));
		return value;
	}

	[[nodiscard]] std::uint32_t dword(std::uint64_t address) const noexcept
	{
		std::uint32_t value{};
		std::memcpy(&value, bytes_.data() + (address - base_), sizeof(value));
		return value;
	}

	[[nodiscard]] std::uint8_t byte(std::uint64_t address) const noexcept
	{
		return static_cast<std::uint8_t>(bytes_[address - base_]);
	}

	[[nodiscard]] std::size_t fetches() const noexcept { return fetches_; }
	void reset_fetches() noexcept { fetches_ = 0; }
	[[nodiscard]] std::size_t atomic_pointers() const noexcept
	{
		return atomic_pointers_.load(std::memory_order_relaxed);
	}
	void reset_atomic_pointers() noexcept
	{
		atomic_pointers_.store(0, std::memory_order_relaxed);
	}
	void set_atomic_pointer_available(bool available) noexcept
	{
		atomic_pointer_available_ = available;
	}
	void set_code_stamps_available(bool available) noexcept
	{
		code_stamps_available_ = available;
	}
	[[nodiscard]] std::size_t code_stamp_queries() const noexcept
	{
		return code_stamp_queries_;
	}
	void reset_code_stamp_queries() noexcept { code_stamp_queries_ = 0; }
	void set_code_changes(std::uint64_t *code_changes) noexcept
	{
		code_changes_ = code_changes;
	}
	[[nodiscard]] std::size_t invalidations() const noexcept { return invalidations_; }
	[[nodiscard]] std::size_t executable_queries() const noexcept
	{
		return executable_queries_.load(std::memory_order_relaxed);
	}

private:
	void bump_code_stamps(std::uint64_t address, std::size_t size) noexcept
	{
		if (!code_stamps_available_ || size == 0 || address < base_)
			return;
		const auto first = static_cast<std::size_t>((address - base_) / 4096U);
		const auto last = static_cast<std::size_t>(
			(address - base_ + size - 1U) / 4096U);
		for (auto page = first;
		     page <= last && page < code_stamp_versions_.size(); ++page)
			++code_stamp_versions_[page];
	}

	std::uint64_t base_{};
	std::vector<std::byte> bytes_;
	uml::memory::MemoryFault fault_{};
	bool faulted_{};
	std::size_t fetches_{};
	std::size_t invalidations_{};
	std::atomic<std::size_t> atomic_pointers_{};
	bool atomic_pointer_available_{true};
	std::atomic<std::size_t> executable_queries_{};
	std::size_t code_stamp_queries_{};
	std::vector<std::uint64_t> code_stamp_versions_;
	bool code_stamps_available_{};
	std::uint64_t executable_page_{~std::uint64_t{}};
	bool every_page_executable_{};
	std::uint64_t *code_changes_{};
};

struct TestCodeSequences final {
	std::uint64_t mapping_changes{1};
	std::uint64_t code_changes{1};
};

[[nodiscard]] uml::cpu::x86_64::gadgets::ProgramCacheSequences
read_test_code_sequences(void *opaque) noexcept
{
	const auto &sequences = *static_cast<TestCodeSequences *>(opaque);
	return {
		.mapping_changes = sequences.mapping_changes,
		.code_changes = sequences.code_changes,
		.interrupted = false,
	};
}

[[nodiscard]] bool expect(bool condition, const char *message) noexcept
{
	if (!condition)
		std::fprintf(stderr, "x86-64 gadget bootstrap: %s\n", message);
	return condition;
}

[[nodiscard]] bool cache_reuses_and_revalidates_programs()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x01, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
	memory.reset_fetches();
	const auto first = executor.select_program(
		memory, 0, ExecutionMode::long64);
	const auto cold_fetches = memory.fetches();
	if (!expect(first.status == gadgets::BuildStatus::built &&
		first.program != nullptr && !first.cache_hit,
		"cold program selection did not populate the cache"))
		return false;

	memory.reset_fetches();
	const auto second = executor.select_program(
		memory, 0, ExecutionMode::long64);
	if (!expect(second.status == gadgets::BuildStatus::built &&
		second.program != nullptr && second.cache_hit,
		"unchanged program did not hit the cache") ||
		!expect(cold_fetches != 0 && memory.fetches() == 0,
			"unchanged cache hit fetched instruction bytes"))
		return false;

	/* A code epoch can advance without changing this block. As in the arm64
	 * hierarchy, exact validation retains it and records the new epoch once.
	 */
	executor.prepare(memory, 42, 1, 2);
	memory.reset_fetches();
	memory.reset_atomic_pointers();
	const auto revalidated = executor.select_program(
		memory, 0, ExecutionMode::long64);
	if (!expect(revalidated.status == gadgets::BuildStatus::built &&
		revalidated.program != nullptr && revalidated.cache_hit,
		"identical source was not retained after a code epoch") ||
		!expect(memory.atomic_pointers() != 0 && memory.fetches() == 0,
			"exact validation copied directly mapped source bytes"))
		return false;

	/* Keep exact validation available to address-space implementations that
	 * cannot expose a stable page mapping to the cache. */
	memory.set_atomic_pointer_available(false);
	executor.prepare(memory, 42, 1, 3);
	memory.reset_fetches();
	memory.reset_atomic_pointers();
	const auto fallback = executor.select_program(
		memory, 0, ExecutionMode::long64);
	if (!expect(fallback.status == gadgets::BuildStatus::built &&
		fallback.program != nullptr && fallback.cache_hit,
		"fetch fallback did not retain identical source") ||
		!expect(memory.atomic_pointers() != 0 && memory.fetches() != 0,
			"unavailable direct mapping skipped the fetch fallback"))
		return false;
	memory.set_atomic_pointer_available(true);

	memory.load(1, {0x02});
	executor.prepare(memory, 42, 1, 4);
	memory.reset_fetches();
	const auto changed = executor.select_program(
		memory, 0, ExecutionMode::long64);
	if (!expect(changed.status == gadgets::BuildStatus::built &&
		changed.program != nullptr && !changed.cache_hit,
		"changed source bytes reused a stale cached program"))
		return false;
	CpuState cpu{};
	const auto executed = gadgets::execute(
		*changed.program, cpu, memory, executor, 64);
	const auto &diagnostics = executor.cache_diagnostics();
	return expect(executed.event == uml::cpu::EngineEvent::system_call,
		"rebuilt cache program did not reach SYSCALL") &&
		expect(cpu.gpr(Register::rax) == 2,
			"rebuilt cache program retained the old immediate") &&
		expect(diagnostics.translations == 2 &&
			diagnostics.validation_attempts == 3 &&
			diagnostics.validation_hits == 2 &&
			diagnostics.validation_mismatches == 1,
			"cache diagnostics did not describe reuse and replacement");
}

[[nodiscard]] bool cache_validates_inline_source_spans()
{
	using namespace uml::cpu::x86_64;
	constexpr std::array<std::size_t, 13> sizes{
		1, 2, 7, 8, 9, 15, 16, 17, 24, 31, 32, 33, 64};
	TestMemory memory;
	gadgets::Executor executor;
	std::uint64_t changes = 1;
	const auto before = executor.cache_diagnostics();
	for (std::size_t index = 0; index < sizes.size(); ++index) {
		const auto size = sizes[index];
		const auto address = 0x100U + index * 0x100U;
		std::vector<std::byte> source(size, std::byte{0x90});
		if (size == 1) {
			source[0] = std::byte{0xcc};
		} else {
			source[size - 2] = std::byte{0x0f};
			source[size - 1] = std::byte{0x05};
		}
		if (!expect(memory.write(address,
				std::span<const std::byte>{source}),
			"validation-span source did not fit test memory"))
			return false;

		executor.prepare(memory, 42, 1, changes++);
		const auto cold = executor.select_program(
			memory, address, ExecutionMode::long64);
		if (!expect(cold.status == gadgets::BuildStatus::built &&
				cold.program != nullptr && !cold.cache_hit,
			"validation-span source did not populate the cache"))
			return false;

		executor.prepare(memory, 42, 1, changes++);
		const auto identical = executor.select_program(
			memory, address, ExecutionMode::long64);
		if (!expect(identical.status == gadgets::BuildStatus::built &&
				identical.program != nullptr && identical.cache_hit,
			"inline source comparison rejected identical bytes"))
			return false;

		source[0] = size == 1 ? std::byte{0xc3} : std::byte{0x91};
		if (!expect(memory.write(address,
				std::span<const std::byte>{source}),
			"changed validation-span source did not fit test memory"))
			return false;
		executor.prepare(memory, 42, 1, changes++);
		const auto changed = executor.select_program(
			memory, address, ExecutionMode::long64);
		if (!expect(changed.status == gadgets::BuildStatus::built &&
				changed.program != nullptr && !changed.cache_hit,
			"inline source comparison accepted changed bytes"))
			return false;
	}
	const auto &after = executor.cache_diagnostics();
	return expect(after.validation_attempts - before.validation_attempts ==
			2 * sizes.size() &&
			after.validation_hits - before.validation_hits == sizes.size() &&
			after.validation_mismatches - before.validation_mismatches ==
				sizes.size(),
		"inline source comparison diagnostics are incomplete");
}

[[nodiscard]] bool cache_rejects_unrelated_code_pages_by_stamp()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x01, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.load(4096, {0x90});
	memory.set_code_stamps_available(true);
	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
	const auto cold = executor.select_program(
		memory, 0, ExecutionMode::long64);
	if (!expect(cold.status == gadgets::BuildStatus::built &&
		cold.program != nullptr && !cold.cache_hit,
		"code-stamp source did not populate the cache"))
		return false;

	/* A publication to a different physical code page must update the cache
	 * epoch without rereading this program's unchanged bytes. */
	memory.load(4096, {0x91});
	executor.prepare(memory, 42, 1, 2);
	memory.reset_fetches();
	memory.reset_atomic_pointers();
	memory.reset_code_stamp_queries();
	const auto unrelated = executor.select_program(
		memory, 0, ExecutionMode::long64);
	if (!expect(unrelated.status == gadgets::BuildStatus::built &&
		unrelated.program != nullptr && unrelated.cache_hit,
		"unrelated code-page publication discarded a cached program") ||
		!expect(memory.code_stamp_queries() != 0 &&
			memory.atomic_pointers() == 0 && memory.fetches() == 0,
			"matching code-page stamp still reread source bytes"))
		return false;

	/* A publication to the program's own page still falls through to exact
	 * validation and rebuilds when the saved bytes differ. */
	memory.load(1, {0x02});
	executor.prepare(memory, 42, 1, 3);
	memory.reset_atomic_pointers();
	const auto changed = executor.select_program(
		memory, 0, ExecutionMode::long64);
	const auto &diagnostics = executor.cache_diagnostics();
	return expect(changed.status == gadgets::BuildStatus::built &&
		changed.program != nullptr && !changed.cache_hit,
		"changed code-page stamp reused stale translated bytes") &&
		expect(memory.atomic_pointers() != 0,
			"changed code-page stamp skipped exact source validation") &&
		expect(diagnostics.validation_stamp_hits == 1 &&
			diagnostics.validation_mismatches == 1,
			"code-page stamp diagnostics did not describe both paths");
}

[[nodiscard]] bool cache_retires_the_oldest_generation()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x0f, 0x05});
	gadgets::Executor executor;
	for (std::uint64_t generation = 1; generation <= 9; ++generation) {
		executor.prepare(memory, generation, 1, 1);
		const auto selected = executor.select_program(
			memory, 0, ExecutionMode::long64);
		if (!expect(selected.status == gadgets::BuildStatus::built &&
			selected.program != nullptr && !selected.cache_hit,
			"new address space unexpectedly reused another's program"))
			return false;
	}
	if (!expect(executor.resident_generations() == 8,
		"x86 cache did not enforce the arm64 eight-generation limit"))
		return false;
	executor.prepare(memory, 1, 1, 1);
	const auto retired = executor.select_program(
		memory, 0, ExecutionMode::long64);
	return expect(retired.status == gadgets::BuildStatus::built &&
		retired.program != nullptr && !retired.cache_hit,
		"least-recently-current address space was not retired");
}

[[nodiscard]] bool cache_links_common_successors()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x0f, 0x05, 0x0f, 0x05});
	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
	const auto first = executor.select_program(
		memory, 0, ExecutionMode::long64);
	const auto successor = executor.select_program(
		memory, 2, ExecutionMode::long64);
	const auto returned = executor.select_program(
		memory, 0, ExecutionMode::long64);
	const auto before = executor.cache_diagnostics().successor_hits;
	const auto linked = executor.select_program(
		memory, 2, ExecutionMode::long64);
	return expect(first.program != nullptr && successor.program != nullptr &&
		returned.program != nullptr && linked.program != nullptr,
		"successor-link setup did not select both programs") &&
		expect(linked.cache_hit &&
			executor.cache_diagnostics().successor_hits == before + 1,
			"common successor did not bypass the main cache probe");
}

[[nodiscard]] bool cache_chains_direct_branches()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xeb, 0x0e});
	memory.load(16, {
		0xb8, 0x2a, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	/* CMP EAX,EAX; JE 16 exercises the conditional linked entry against the
	 * same active target as the direct JMP above.
	 */
	memory.load(32, {0x39, 0xc0, 0x74, 0xec});
	/* CALL 16 from 48; the return address is 53. */
	memory.load(48, {0xe8, 0xdb, 0xff, 0xff, 0xff});

	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
	const auto target = executor.select_program(
		memory, 16, ExecutionMode::long64);
	const auto direct = executor.select_program(
		memory, 0, ExecutionMode::long64);
	const auto conditional = executor.select_program(
		memory, 32, ExecutionMode::long64);
	const auto call = executor.select_program(
		memory, 48, ExecutionMode::long64);
	if (!expect(target.program != nullptr && direct.program != nullptr &&
			conditional.program != nullptr && call.program != nullptr,
		"direct-link setup did not cache all programs"))
		return false;

	CpuState cpu{};
	const auto chained = gadgets::execute(
		*direct.program, cpu, memory, executor, 64);
	if (!expect(chained.event == uml::cpu::EngineEvent::system_call &&
			chained.retired_instructions == 3 &&
			cpu.gpr(Register::rax) == 42 && cpu.rip == 23,
		"cached direct JMP did not enter its native successor"))
		return false;

	cpu = CpuState{};
	cpu.rip = 32;
	const auto conditionally_chained = gadgets::execute(
		*conditional.program, cpu, memory, executor, 64);
	if (!expect(conditionally_chained.event ==
			uml::cpu::EngineEvent::system_call &&
			conditionally_chained.retired_instructions == 4 &&
			cpu.gpr(Register::rax) == 42 && cpu.rip == 23,
		"cached JE did not enter its native successor"))
		return false;

	cpu = CpuState{};
	cpu.rip = 48;
	cpu.gpr(Register::rsp) = 512;
	const auto call_chained = gadgets::execute(
		*call.program, cpu, memory, executor, 64);
	if (!expect(call_chained.event == uml::cpu::EngineEvent::system_call &&
			call_chained.retired_instructions == 3 && cpu.rip == 23 &&
			cpu.gpr(Register::rsp) == 504 && memory.qword(504) == 53,
		"cached direct CALL did not push its return address and enter its "
		"native callee"))
		return false;

	cpu = CpuState{};
	const auto bounded = gadgets::execute(
		*direct.program, cpu, memory, executor, 1);
	if (!expect(bounded.event == uml::cpu::EngineEvent::continued &&
			bounded.retired_instructions == 1 &&
			bounded.instruction_budget_exhausted && cpu.rip == 16,
		"native direct link crossed the exact instruction budget"))
		return false;

	/* A code epoch resets active native links before exact validation. The
	 * source may be retained immediately, but it must stop at the reset target
	 * until that target is itself validated and activated again.
	 */
	executor.prepare(memory, 42, 1, 2);
	const auto source_after_change = executor.select_program(
		memory, 0, ExecutionMode::long64);
	if (!expect(source_after_change.program != nullptr &&
			source_after_change.cache_hit,
		"unchanged direct-link source did not survive exact validation"))
		return false;
	cpu = CpuState{};
	const auto reset = gadgets::execute(
		*source_after_change.program, cpu, memory, executor, 64);
	if (!expect(reset.event == uml::cpu::EngineEvent::continued &&
			reset.retired_instructions == 1 && cpu.rip == 16,
		"code epoch left a stale native target active"))
		return false;

	const auto target_after_change = executor.select_program(
		memory, 16, ExecutionMode::long64);
	cpu = CpuState{};
	const auto restored = gadgets::execute(
		*source_after_change.program, cpu, memory, executor, 64);
	return expect(target_after_change.program != nullptr &&
			target_after_change.cache_hit,
			"unchanged native target did not survive exact validation") &&
		expect(restored.event == uml::cpu::EngineEvent::system_call &&
			restored.retired_instructions == 3 && cpu.rip == 23,
			"validated native target was not reactivated");
}

/* Every indirect branch in an address space shares one tier keyed on the
 * target, and a block publishes itself into that tier when the cache hands it
 * out. So a RET enters a return site it has never returned to, and a site that
 * has already spent both of its private entries still reaches a third target
 * without leaving the stream -- neither of which a tier keyed on the branch
 * can do, because the addresses a function returns to belong to its callers.
 * The private entries stay the first tier and still carry a site's own targets
 * when the shared one evicts them.
 */
[[nodiscard]] bool cache_chains_indirect_transfers()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xff, 0xe0});
	memory.load(16, {
		0xb8, 0x2a, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.load(24, {
		0xb8, 0x2b, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.load(32, {0xc3});
	memory.load(40, {
		0xb8, 0x2c, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.load(48, {0xff, 0xd0});
	/* Folds onto the same shared-tier entry as 16. */
	memory.load(0x1011, {
		0xb8, 0x2d, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	memory.load(512, {0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});

	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
	const auto jump = executor.select_program(
		memory, 0, ExecutionMode::long64);
	const auto ret = executor.select_program(
		memory, 32, ExecutionMode::long64);
	const auto call = executor.select_program(
		memory, 48, ExecutionMode::long64);
	if (!expect(jump.program != nullptr && ret.program != nullptr &&
			call.program != nullptr,
		"indirect-link setup did not cache its branch programs"))
		return false;

	CpuState cpu{};
	cpu.gpr(Register::rax) = 16;
	const auto cold = gadgets::execute(
		*jump.program, cpu, memory, executor, 64);
	if (!expect(cold.event == uml::cpu::EngineEvent::continued &&
			cold.retired_instructions == 1 && cpu.rip == 16,
		"indirect JMP entered a target no block had run at"))
		return false;

	const auto target_a = executor.select_program(
		memory, 16, ExecutionMode::long64);
	const auto target_b = executor.select_program(
		memory, 24, ExecutionMode::long64);
	const auto target_c = executor.select_program(
		memory, 40, ExecutionMode::long64);
	if (!expect(target_a.program != nullptr && target_b.program != nullptr &&
			target_c.program != nullptr,
		"indirect-link setup did not cache all three targets"))
		return false;

	/* Three targets through a two-entry site, revisited: the third would have
	 * evicted one of the first two from the site alone. */
	struct Visit final {
		std::uint64_t target;
		std::uint64_t result;
		std::uint64_t next;
	};
	static constexpr Visit visits[] = {
		{16, 42, 23}, {24, 43, 31}, {40, 44, 47}, {16, 42, 23},
		{24, 43, 31},
	};
	for (const auto &visit : visits) {
		cpu = CpuState{};
		cpu.gpr(Register::rax) = visit.target;
		const auto entered = gadgets::execute(
			*jump.program, cpu, memory, executor, 64);
		if (!expect(entered.event == uml::cpu::EngineEvent::system_call &&
				entered.retired_instructions == 3 &&
				cpu.gpr(Register::rax) == visit.result &&
				cpu.rip == visit.next,
			"indirect JMP left the stream for a target that had run"))
			return false;
	}

	/* This RET has never returned anywhere, and it still enters its return
	 * site, because the site announced itself when the cache handed it out. */
	cpu = CpuState{};
	cpu.rip = 32;
	cpu.gpr(Register::rsp) = 512;
	const auto returned = gadgets::execute(
		*ret.program, cpu, memory, executor, 64);
	if (!expect(returned.event == uml::cpu::EngineEvent::system_call &&
			returned.retired_instructions == 3 && cpu.rip == 23 &&
			cpu.gpr(Register::rsp) == 520,
		"RET left the stream for a return site that had run"))
		return false;

	cpu = CpuState{};
	cpu.gpr(Register::rax) = 16;
	const auto bounded = gadgets::execute(
		*jump.program, cpu, memory, executor, 1);
	if (!expect(bounded.event == uml::cpu::EngineEvent::continued &&
			bounded.retired_instructions == 1 &&
			bounded.instruction_budget_exhausted && cpu.rip == 16,
		"native indirect link crossed the exact instruction budget"))
		return false;

	cpu = CpuState{};
	cpu.rip = 48;
	cpu.gpr(Register::rax) = 16;
	cpu.gpr(Register::rsp) = 1024;
	const auto called = gadgets::execute(
		*call.program, cpu, memory, executor, 64);
	if (!expect(called.event == uml::cpu::EngineEvent::system_call &&
			called.retired_instructions == 3 && cpu.rip == 23,
		"indirect CALL left the stream for a target that had run") ||
		!expect(cpu.gpr(Register::rsp) == 1016 &&
			memory.qword(1016) == 50,
			"indirect CALL lost its architectural stack write"))
		return false;

	/* Evict the shared entry 16 answers to. The JMP trained its own site on
	 * the cold miss above, so its private tier still carries the target. */
	const auto conflicting = executor.select_program(
		memory, 0x1011, ExecutionMode::long64);
	if (!expect(conflicting.program != nullptr,
		"conflicting target did not cache"))
		return false;
	cpu = CpuState{};
	cpu.gpr(Register::rax) = 16;
	const auto retained = gadgets::execute(
		*jump.program, cpu, memory, executor, 64);
	return expect(retained.event == uml::cpu::EngineEvent::system_call &&
			retained.retired_instructions == 3 &&
			cpu.gpr(Register::rax) == 42 && cpu.rip == 23,
		"a shared-tier eviction cost a site its own trained target");
}

/* No other test here binds an execution context, so `code_link_safe` has never
 * been reachable: without one it refuses every link and the engine leaves the
 * stream. The two call tests below are about exactly that decision, so they
 * supply the sequence contract a real address space provides.
 */
struct TestSequences final {
	std::uint64_t mapping_changes{1};
	std::uint64_t code_changes{1};
	std::array<uml::cpu::x86_64::gadgets::ProgramCodeChange, 8> history{};
	std::size_t recorded{};

	void change(std::uint64_t address, std::uint64_t length) noexcept
	{
		if (recorded < history.size())
			history[recorded] = {address, length};
		++recorded;
		++code_changes;
	}
};

[[nodiscard]] uml::cpu::x86_64::gadgets::ProgramCacheSequences
read_test_sequences(void *context) noexcept
{
	const auto &state = *static_cast<const TestSequences *>(context);
	return {state.mapping_changes, state.code_changes, false};
}

/* Stands in for the address space's own history: the ranges published after
 * `sequence`, or a refusal when it cannot describe the whole span. */
[[nodiscard]] int read_test_code_changes(void *context, std::uint64_t sequence,
	uml::cpu::x86_64::gadgets::ProgramCodeChange *changes,
	std::size_t capacity, std::uint64_t *current_sequence) noexcept
{
	const auto &state = *static_cast<const TestSequences *>(context);
	*current_sequence = state.code_changes;
	if (sequence >= state.code_changes)
		return 0;
	if (state.recorded > state.history.size() ||
		sequence + state.recorded < state.code_changes)
		return -1;
	const auto count = state.code_changes - sequence;
	if (count > capacity)
		return -1;
	for (std::uint64_t index = 0; index < count; ++index)
		changes[index] = state.history[static_cast<std::size_t>(
			state.recorded - count + index)];
	return static_cast<int>(count);
}

/* A call whose return-address push lands on an executable page publishes that
 * write like any other store, and then follows its link. What makes that safe
 * is not the page but the bytes: publication deactivates every cached block the
 * write actually overlapped and resets its link, and the link itself still has
 * to pass `code_link_safe`. Leaving the stream merely because some executable
 * page was written is what an ordinary store already declines to do, and a call
 * that did it anyway left the engine paying a C++ round trip for the majority
 * of the control transfers a real program performs.
 */
/* A guest code write says nothing on its own about the blocks this cache
 * holds. Cooling every link in the address space for one is what made a JIT or
 * a fresh executable mapping cost a C++ round trip for every edge afterwards,
 * however far the write was from anything translated. The address space knows
 * which bytes moved, and retiring exactly the translations covering them is
 * the same selective retirement the cache already performs for a write it made
 * itself.
 */
[[nodiscard]] bool an_unrelated_code_change_leaves_a_link_armed()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xe9, 0x0b, 0x00, 0x00, 0x00});
	memory.load(16, {0x0f, 0x05});
	TestSequences sequences;
	memory.set_code_changes(&sequences.code_changes);
	const auto prepare = [&](gadgets::Executor &executor) {
		executor.prepare(memory, 42, sequences.mapping_changes,
			sequences.code_changes, &sequences, read_test_sequences,
			read_test_code_changes);
	};
	/* Only the jump is selected before each run. Selecting the target would
	 * re-arm its link, which is what the dispatch loop does and what would
	 * hide the difference this test is about. */
	const auto seed = [&](gadgets::Executor &executor) {
		return executor.select_program(memory, 16,
			ExecutionMode::long64).program != nullptr;
	};
	const auto run = [&](gadgets::Executor &executor) {
		const auto jump = executor.select_program(
			memory, 0, ExecutionMode::long64);
		if (jump.program == nullptr)
			return gadgets::ExecuteResult{};
		CpuState cpu{};
		return gadgets::execute(*jump.program, cpu, memory, executor, 64);
	};

	gadgets::Executor executor;
	prepare(executor);
	if (!expect(seed(executor), "linked jump target did not cache"))
		return false;
	const auto armed = run(executor);
	if (!expect(armed.event == uml::cpu::EngineEvent::system_call &&
			armed.retired_instructions == 2,
		"linked jump did not enter its target"))
		return false;

	/* A page neither block was translated from. */
	sequences.change(0x2000, 4096);
	prepare(executor);
	const auto unrelated = run(executor);
	if (!expect(unrelated.event == uml::cpu::EngineEvent::system_call &&
			unrelated.retired_instructions == 2,
		"a code change on an untranslated page retired a link"))
		return false;

	/* The page the target was translated from. */
	sequences.change(0, 4096);
	prepare(executor);
	const auto covered = run(executor);
	if (!expect(covered.event == uml::cpu::EngineEvent::continued &&
			covered.retired_instructions == 1,
		"a code change covering the target left its link armed"))
		return false;

	/* And a history that cannot describe the span retires everything. */
	gadgets::Executor blind;
	prepare(blind);
	if (!expect(seed(blind) &&
			run(blind).event == uml::cpu::EngineEvent::system_call,
		"linked jump did not enter its target for the second executor"))
		return false;
	sequences.recorded = sequences.history.size() + 1;
	sequences.change(0x2000, 4096);
	prepare(blind);
	return expect(run(blind).event == uml::cpu::EngineEvent::continued,
		"an undescribed code change left a link armed");
}

[[nodiscard]] bool linked_call_crosses_a_disjoint_executable_stack_write()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xe8, 0x0b, 0x00, 0x00, 0x00});
	memory.load(16, {0x0f, 0x05});
	memory.set_executable_page(0);
	TestSequences sequences;
	memory.set_code_changes(&sequences.code_changes);
	gadgets::Executor executor;
	executor.prepare(memory, 42, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_test_sequences);
	const auto target = executor.select_program(
		memory, 16, ExecutionMode::long64);
	const auto call = executor.select_program(
		memory, 0, ExecutionMode::long64);
	if (!expect(target.program != nullptr && call.program != nullptr,
		"executable-stack CALL did not populate its direct link"))
		return false;

	CpuState cpu{};
	cpu.gpr(Register::rsp) = 0x800;
	const auto result = gadgets::execute(
		*call.program, cpu, memory, executor, 64);
	return expect(result.event == uml::cpu::EngineEvent::system_call &&
			result.retired_instructions == 2 && cpu.rip == 18,
		"direct CALL left the stream for a stack write that changed "
		"no instruction it was about to run") &&
		expect(cpu.gpr(Register::rsp) == 0x7f8 &&
			memory.qword(0x7f8) == 5 && memory.invalidations() == 1,
			"code-changing CALL lost its architectural stack write");
}

/* The same call, with the push aimed at the callee's own instructions. Nothing
 * about the call gadget stops this one; publication deactivates the block the
 * write overlapped and resets the link it was reached through, so following
 * that link finds the fallback and returns to have the callee retranslated.
 */
[[nodiscard]] bool linked_call_stops_when_the_stack_write_hits_its_target()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xe8, 0x0b, 0x00, 0x00, 0x00});
	memory.load(16, {0x0f, 0x05});
	memory.set_executable_page(0);
	TestSequences sequences;
	memory.set_code_changes(&sequences.code_changes);
	gadgets::Executor executor;
	executor.prepare(memory, 42, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_test_sequences);
	const auto target = executor.select_program(
		memory, 16, ExecutionMode::long64);
	const auto call = executor.select_program(
		memory, 0, ExecutionMode::long64);
	if (!expect(target.program != nullptr && call.program != nullptr,
		"overlapping-stack CALL did not populate its direct link"))
		return false;

	CpuState cpu{};
	/* The eight-byte push lands on 16, which is the callee. */
	cpu.gpr(Register::rsp) = 24;
	const auto result = gadgets::execute(
		*call.program, cpu, memory, executor, 64);
	return expect(result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 1 && cpu.rip == 16,
		"direct CALL entered a callee its own stack write had just "
		"overwritten") &&
		expect(cpu.gpr(Register::rsp) == 16 &&
			memory.qword(16) == 5 && memory.invalidations() == 1,
			"overlapping-stack CALL lost its architectural stack write");
}

[[nodiscard]] bool executable_store_publishes_code_change()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	/* Two MOV byte ptr stores followed by SYSCALL. Both destinations share the
	 * code page but do not overlap this translated block.
	 */
	memory.load(0, {0xc6, 0x00, 0x7f, 0xc6, 0x40, 0x01, 0x6e, 0x0f, 0x05});
	memory.set_executable_page(0);
	CpuState cpu{};
	cpu.gpr(Register::rax) = 0x800;
	gadgets::Executor executor;
	executor.prepare(memory, 1, 1, 1);
	/* Cached, so the page is on the shared code-page list and its stores
	 * have a translation to protect. */
	const auto selected = executor.select_program(
		memory, 0, ExecutionMode::long64);
	if (!expect(selected.program != nullptr,
		"executable-store block did not build"))
		return false;
	const auto result = gadgets::execute(
		*selected.program, cpu, memory, executor, 64);
	return expect(result.event == uml::cpu::EngineEvent::system_call &&
		result.retired_instructions == 3 && cpu.rip == 9,
		"non-overlapping executable stores did not stay in one native block") &&
		expect(memory.invalidations() == 1,
			"batched executable stores did not publish one code change") &&
		expect(memory.byte(0x800) == 0x7f && memory.byte(0x801) == 0x6e,
			"batched executable stores did not update guest memory");
}

/* A store publishes a code change to protect cached translations, and only
 * the executors know where those are. With every page executable, a store to
 * a page no executor has translated publishes nothing; once another executor
 * caches code on that page, this executor's next store to it publishes even
 * though its own write translation was filled before the page gained code.
 */
[[nodiscard]] bool store_publishes_only_where_some_executor_caches_code()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	/* Page 0: MOV byte ptr [RAX], 0x7f; SYSCALL. Page 1: SYSCALL. */
	memory.load(0, {0xc6, 0x00, 0x7f, 0x0f, 0x05});
	memory.load(0x1000, {0x0f, 0x05});
	memory.set_every_page_executable(true);
	gadgets::Executor writer;
	writer.prepare(memory, 7, 1, 1);
	const auto block = writer.select_program(
		memory, 0, ExecutionMode::long64);
	if (!expect(block.program != nullptr, "code-page registry block did not build"))
		return false;
	CpuState cpu{};
	cpu.gpr(Register::rax) = 0x1800;
	auto result = gadgets::execute(*block.program, cpu, memory, writer, 64);
	if (!expect(result.event == uml::cpu::EngineEvent::system_call &&
		memory.byte(0x1800) == 0x7f,
		"store to an untranslated executable page did not run") ||
		!expect(memory.invalidations() == 0,
			"store to a page holding no cached code published a change"))
		return false;

	gadgets::Executor reader;
	reader.prepare(memory, 7, 1, 1);
	const auto target = reader.select_program(
		memory, 0x1000, ExecutionMode::long64);
	if (!expect(target.program != nullptr, "code-page registry target did not build"))
		return false;
	cpu = {};
	cpu.rip = 0;
	cpu.gpr(Register::rax) = 0x1801;
	result = gadgets::execute(*block.program, cpu, memory, writer, 64);
	if (!expect(result.event == uml::cpu::EngineEvent::system_call &&
		memory.byte(0x1801) == 0x7f,
		"store to a newly translated page did not run") ||
		!expect(memory.invalidations() == 1,
			"store to a page another executor translated did not publish"))
		return false;

	/* Retiring the reader's translations takes the page off the list again. */
	reader = gadgets::Executor{};
	cpu = {};
	cpu.rip = 0;
	cpu.gpr(Register::rax) = 0x1802;
	result = gadgets::execute(*block.program, cpu, memory, writer, 64);
	return expect(result.event == uml::cpu::EngineEvent::system_call &&
		memory.byte(0x1802) == 0x7f,
		"store after the other executor retired did not run") &&
		expect(memory.invalidations() == 1,
			"store to a page that lost its cached code still published");
}

[[nodiscard]] bool overlapping_executable_store_stops_immediately()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	/* MOV byte ptr [RAX], 0x90; SYSCALL. Replacing the first SYSCALL byte
	 * must return before the stale translated instruction can execute.
	 */
	memory.load(0, {0xc6, 0x00, 0x90, 0x0f, 0x05});
	memory.set_executable_page(0);
	const auto built = gadgets::build(memory, 0);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"overlapping executable-store block did not build"))
		return false;
	CpuState cpu{};
	cpu.gpr(Register::rax) = 3;
	gadgets::Executor executor;
	executor.prepare(memory, 1, 1, 1);
	const auto result = gadgets::execute(
		built.program, cpu, memory, executor, 64);
	return expect(result.event == uml::cpu::EngineEvent::continued &&
		result.retired_instructions == 1 && cpu.rip == 3,
		"overlapping executable store ran stale translated code") &&
		expect(memory.invalidations() == 1,
			"overlapping executable store did not publish its change") &&
		expect(memory.byte(3) == 0x90,
			"overlapping executable store did not update guest memory");
}

[[nodiscard]] bool executable_store_links_only_to_unchanged_code()
{
	using namespace uml::cpu::x86_64;
	const auto run = [](std::uint64_t destination, bool should_chain,
		bool concurrent_change = false) {
		TestMemory memory;
		/* MOV byte ptr [RAX], 0x90; JMP 16. The cached target is a
		 * SYSCALL and both mappings deliberately share one executable page.
		 */
		memory.load(0, {
			0xc6, 0x00, 0x90,
			0xe9, 0x08, 0x00, 0x00, 0x00,
		});
		memory.load(16, {0x0f, 0x05});
		memory.set_executable_page(0);
		TestCodeSequences sequences;
		memory.set_code_changes(&sequences.code_changes);
		gadgets::Executor executor;
		executor.prepare(memory, 42, sequences.mapping_changes,
			sequences.code_changes, &sequences,
			read_test_code_sequences);
		const auto target = executor.select_program(
			memory, 16, ExecutionMode::long64);
		const auto source = executor.select_program(
			memory, 0, ExecutionMode::long64);
		if (!expect(target.program != nullptr && source.program != nullptr,
			"code-write link setup did not cache both programs"))
			return false;
		if (concurrent_change)
			++sequences.code_changes;

		CpuState cpu{};
		cpu.gpr(Register::rax) = destination;
		const auto result = gadgets::execute(
			*source.program, cpu, memory, executor, 64);
		if (should_chain)
			return expect(
				result.event == uml::cpu::EngineEvent::system_call &&
				result.retired_instructions == 3 && cpu.rip == 18,
				"non-overlapping code write did not retain its safe link") &&
				expect(memory.invalidations() == 1 &&
					sequences.code_changes == 2,
					"safe code-write link published the wrong sequence");
		return expect(result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 2 && cpu.rip == 16,
			"unsafe code write entered a cached target") &&
			expect(memory.invalidations() == 1,
				"unsafe code-write link did not publish exactly once") &&
			expect(destination != 16 || memory.byte(16) == 0x90,
				"overlapping target write was not committed");
	};
	return run(0x800, true) && run(16, false) &&
		run(0x800, false, true);
}

/* A store into a cached block far outside the running program is caught by
 * publication, not by the program-range check, which only sees the block being
 * executed. Reselecting a block revalidates it against its source bytes, so the
 * stale case needs an already-linked edge that is followed without reselection:
 * run once with a store that changes nothing so the link forms, then again with
 * a store that rewrites the target.
 */
[[nodiscard]] bool distant_code_write_cools_its_linked_block()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t target = 0x800;
	TestMemory memory;
	/* MOV [RBX], AL; JMP target. */
	memory.load(0, {0x88, 0x03, 0xe9, 0xf9, 0x07, 0x00, 0x00});
	/* MOV EAX, 1; SYSCALL. The second run rewrites the immediate to 2. */
	memory.load(target, {0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05});
	memory.set_executable_page(0);
	TestCodeSequences sequences;
	memory.set_code_changes(&sequences.code_changes);
	gadgets::Executor executor;
	executor.prepare(memory, 42, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_test_code_sequences);
	if (!expect(executor.select_program(memory, target,
			ExecutionMode::long64).program != nullptr &&
		executor.select_program(memory, 0,
			ExecutionMode::long64).program != nullptr,
		"distant code-write setup did not cache both programs"))
		return false;

	const auto run = [&](std::uint8_t value) {
		const auto source = executor.select_program(
			memory, 0, ExecutionMode::long64);
		if (source.program == nullptr)
			return CpuState{};
		CpuState cpu{};
		cpu.gpr(Register::rbx) = target + 1;
		cpu.gpr(Register::rax) = value;
		(void)gadgets::execute(*source.program, cpu, memory, executor, 64);
		return cpu;
	};

	/* Writing back the byte already there leaves the target unchanged, so the
	 * edge is safe to keep and the link is established.
	 */
	const auto warm = run(0x01);
	if (!expect(memory.byte(target + 1) == 0x01,
			"link-forming run did not perform its store") ||
		!expect(warm.gpr(Register::rax) == 1,
			"link-forming run did not reach the cached target"))
		return false;

	const auto after = run(0x02);
	if (!expect(memory.byte(target + 1) == 0x02,
		"distant code write was not committed"))
		return false;
	if (after.rip == target)
		/* Stopping at the boundary is correct: the next selection rebuilds. */
		return true;
	return expect(after.gpr(Register::rax) == 2,
		"a linked edge entered a stale translation of a written block");
}

[[nodiscard]] bool code_write_resets_unvalidated_successors()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	/* Source writes over the second target and branches to an unchanged
	 * intermediary. The intermediary's cached edge must be reset even though
	 * the immediate source-to-intermediary edge is safe to reactivate.
	 */
	memory.load(0, {
		0xc6, 0x00, 0x90,
		0xe9, 0x08, 0x00, 0x00, 0x00,
	});
	memory.load(16, {
		0xe9, 0x0b, 0x00, 0x00, 0x00,
	});
	memory.load(32, {0x0f, 0x05});
	memory.set_executable_page(0);
	TestCodeSequences sequences;
	memory.set_code_changes(&sequences.code_changes);
	gadgets::Executor executor;
	executor.prepare(memory, 42, sequences.mapping_changes,
		sequences.code_changes, &sequences, read_test_code_sequences);
	const auto final_target = executor.select_program(
		memory, 32, ExecutionMode::long64);
	const auto intermediary = executor.select_program(
		memory, 16, ExecutionMode::long64);
	const auto source = executor.select_program(
		memory, 0, ExecutionMode::long64);
	if (!expect(final_target.program != nullptr &&
		intermediary.program != nullptr && source.program != nullptr,
		"code-write successor reset setup did not cache every program"))
		return false;

	CpuState cpu{};
	cpu.gpr(Register::rax) = 32;
	const auto result = gadgets::execute(
		*source.program, cpu, memory, executor, 64);
	return expect(result.event == uml::cpu::EngineEvent::continued &&
		result.retired_instructions == 3 && cpu.rip == 32,
		"code write followed an unvalidated stale successor") &&
		expect(memory.byte(32) == 0x90 && memory.invalidations() == 1,
			"code-write successor reset lost the guest store");
}

[[nodiscard]] bool slow_executable_writes_publish_once_per_native_run()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.set_executable_page(0);
	std::array<std::byte, 16> first{};
	std::array<std::byte, 16> second{};
	first.fill(std::byte{0x55});
	second.fill(std::byte{0xaa});
	gadgets::NativeExecutionContext context{
		.memory = &memory,
	};
	const auto first_result = gadgets::uml_x86_64_gadget_bulk_store(
		&context, 0x800, first.data(), first.size());
	const auto second_result = gadgets::uml_x86_64_gadget_bulk_store(
		&context, 0x810, second.data(), second.size());
	return expect(first_result == 1 && second_result == 1,
		"slow executable stores did not complete") &&
		expect(memory.invalidations() == 1 &&
			context.code_publications == 1,
			"slow executable stores repeated one native publication") &&
		expect(memory.executable_queries() == 1,
			"published slow store repeated its executable-page probe") &&
		expect(memory.byte(0x800) == 0x55 && memory.byte(0x810) == 0xaa,
			"slow executable stores lost guest bytes");
}

[[nodiscard]] bool decode_scaled_address()
{
	TestMemory memory;
	memory.load(0, {0x48, 0x8b, 0x44, 0x8d, 0xf0});
	const auto result = uml::cpu::x86_64::decode(memory, 0);
	const auto &source = result.instruction.source;
	return expect(result.status == uml::cpu::x86_64::DecodeStatus::decoded,
		"scaled load did not decode") &&
		expect(result.instruction.length == 5, "scaled load length is wrong") &&
		expect(source.kind == uml::cpu::x86_64::OperandKind::memory,
			"scaled load source is not memory") &&
		expect(source.memory.base == 5 && source.memory.index == 1 &&
			source.memory.scale == 4 && source.memory.displacement == -16,
			"ModR/M plus SIB address was not normalized");
}

[[nodiscard]] bool execute_scalar_control_flow()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x48, 0xb8, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x48, 0x83, 0xc0, 0x03,
		0x48, 0x83, 0xf8, 0x08,
		0x74, 0x07,
		0x48, 0xc7, 0xc7, 0x00, 0x00, 0x00, 0x00,
		0x48, 0xc7, 0xc7, 0x2a, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	CpuState cpu{};
	const auto first = gadgets::build(memory, 0);
	if (!expect(first.status == gadgets::BuildStatus::built,
		"scalar block did not build"))
		return false;
	const auto first_run = gadgets::execute(first.program, cpu, memory, 64);
	if (!expect(first_run.event == uml::cpu::EngineEvent::continued,
		"conditional block returned the wrong event") ||
		!expect(first_run.retired_instructions == 4,
			"conditional block retirement is wrong") ||
		!expect(cpu.gpr(Register::rax) == 8 && cpu.rip == 27,
			"arithmetic or equal branch result is wrong") ||
		!expect(cpu.deferred_flags.operation ==
				DeferredFlagOperation::subtract,
			"equal branch eagerly materialized the deferred flags"))
		return false;

	const auto second = gadgets::build(memory, cpu.rip);
	if (!expect(second.status == gadgets::BuildStatus::built,
		"syscall block did not build"))
		return false;
	const auto second_run = gadgets::execute(second.program, cpu, memory, 64);
	return expect(second_run.event == uml::cpu::EngineEvent::system_call,
		"SYSCALL did not exit the gadget stream") &&
		expect(second_run.retired_instructions == 2,
			"SYSCALL block retirement is wrong") &&
		expect(cpu.gpr(Register::rdi) == 42 && cpu.rip == 36,
			"SYSCALL state is wrong") &&
		expect(cpu.gpr(Register::rcx) == 36 &&
			cpu.gpr(Register::r11) == cpu.rflags,
			"SYSCALL did not publish RCX/R11");
}

[[nodiscard]] bool execute_absolute_memory_test_branch()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t base = 0x7ffe0000;
	constexpr std::uint64_t code = base + 0x100;
	constexpr std::uint64_t shared_data = base + 0x308;
	TestMemory memory{base};
	memory.load(code, {
		0xf6, 0x04, 0x25, 0x08, 0x03, 0xfe, 0x7f, 0x01,
		0x75, 0x05,
	});
	memory.load(shared_data, {0x01});
	memory.load(code + 15, {
		0xb8, 0x2a, 0x00, 0x00, 0x00,
		0x0f, 0x05,
	});
	const auto block = gadgets::build(memory, code);
	if (!expect(block.status == gadgets::BuildStatus::built,
		"absolute TEST/JNE block did not build"))
		return false;

	CpuState cpu{};
	cpu.rip = code;
	const auto result = gadgets::execute(block.program, cpu, memory, 2);
	if (!expect(result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 2,
			"absolute TEST/JNE block did not complete") &&
		expect(cpu.rip == code + 15,
			"absolute TEST/JNE ignored a set tested bit") &&
		expect(cpu.deferred_flags.operation == DeferredFlagOperation::logical &&
			cpu.deferred_flags.result == 1,
			"absolute TEST recorded the wrong deferred result"))
		return false;

	/* Wine's syscall stubs use this exact TEST/JNE sequence.  Exercise it
	 * through the production cache too, with the taken target already linked,
	 * so a direct build cannot hide a conditional-link regression.
	 */
	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
	const auto target = executor.select_program(
		memory, code + 15, ExecutionMode::long64);
	const auto source = executor.select_program(
		memory, code, ExecutionMode::long64);
	if (!expect(target.program != nullptr && source.program != nullptr,
		"absolute TEST/JNE cache setup did not build both blocks"))
		return false;
	cpu = CpuState{};
	cpu.rip = code;
	const auto linked = gadgets::execute(
		*source.program, cpu, memory, executor, 64);
	return expect(linked.event == uml::cpu::EngineEvent::system_call &&
			linked.retired_instructions == 4 &&
			cpu.gpr(Register::rax) == 42 && cpu.rip == code + 22,
			"cached absolute TEST/JNE did not enter its linked target");
}

[[nodiscard]] bool execute_lazy_zero_branches()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x74, 0x05});
	memory.load(16, {0x75, 0x05});
	const auto equal = gadgets::build(memory, 0);
	const auto not_equal = gadgets::build(memory, 16);
	if (!expect(equal.status == gadgets::BuildStatus::built &&
			not_equal.status == gadgets::BuildStatus::built,
		"zero-condition branches did not build"))
		return false;

	const auto run_branch = [&](const gadgets::Program &program, CpuState cpu,
		std::uint64_t expected_rip,
		DeferredFlagOperation expected_operation,
		const char *message) {
		cpu.rip = program.address();
		const auto result = gadgets::execute(program, cpu, memory, 1);
		return expect(result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 1 && cpu.rip == expected_rip &&
			cpu.deferred_flags.operation == expected_operation, message);
	};

	CpuState cpu{};
	cpu.rflags &= ~rflags_zero;
	cpu.deferred_flags.result = 0;
	cpu.deferred_flags.operation = DeferredFlagOperation::subtract;
	cpu.deferred_flags.width = 64;
	if (!run_branch(equal.program, cpu, 7,
			DeferredFlagOperation::subtract,
			"JE did not take a deferred zero result") ||
		!run_branch(not_equal.program, cpu, 18,
			DeferredFlagOperation::subtract,
			"JNE took a deferred zero result"))
		return false;

	cpu.rflags |= rflags_zero;
	cpu.deferred_flags.result = 1;
	if (!run_branch(equal.program, cpu, 2,
			DeferredFlagOperation::subtract,
			"JE took a deferred nonzero result") ||
		!run_branch(not_equal.program, cpu, 23,
			DeferredFlagOperation::subtract,
			"JNE did not take a deferred nonzero result"))
		return false;

	cpu = CpuState{};
	cpu.rflags |= rflags_zero;
	if (!run_branch(equal.program, cpu, 7, DeferredFlagOperation::none,
			"JE ignored an architectural zero flag") ||
		!run_branch(not_equal.program, cpu, 18,
			DeferredFlagOperation::none,
			"JNE ignored an architectural zero flag"))
		return false;

	cpu.rflags &= ~rflags_zero;
	return run_branch(equal.program, cpu, 2, DeferredFlagOperation::none,
			"JE ignored a clear architectural zero flag") &&
		run_branch(not_equal.program, cpu, 23,
			DeferredFlagOperation::none,
			"JNE ignored a clear architectural zero flag");
}

/* glibc's i386 futex wait rejects a positive syscall result with this exact
 * TEST EAX,EAX; JG sequence.  The kernel bridge stores syscall returns in the
 * 64-bit backing slot, so retain upper garbage here and prove that compat32
 * flags and the signed branch use only the architectural EAX value.
 */
[[nodiscard]] bool materialized_flags_match_the_reference()
{
	using namespace uml::cpu::x86_64;
	/* The engine folds the recorded operation and width into one body per
	 * pair, so every pair has to agree with the independent C++
	 * implementation rather than only with the pairs a corpus happens to
	 * reach. PUSHFQ is the one instruction that exposes the whole result.
	 */
	TestMemory memory;
	memory.load(0, {0x9c});
	const auto block = gadgets::build(memory, 0);
	if (!expect(block.status == gadgets::BuildStatus::built,
		"PUSHFQ block did not build"))
		return false;

	constexpr std::uint64_t stack = 0x800;
	constexpr std::array<DeferredFlagOperation, 7> operations{
		DeferredFlagOperation::add,
		DeferredFlagOperation::subtract,
		DeferredFlagOperation::logical,
		DeferredFlagOperation::increment,
		DeferredFlagOperation::decrement,
		DeferredFlagOperation::add_carry,
		DeferredFlagOperation::subtract_borrow,
	};
	constexpr std::array<std::uint8_t, 4> widths{1, 2, 4, 8};
	constexpr std::array<std::uint64_t, 8> values{
		0, 1, 0x7f, 0x80, 0xff, 0x7fffffff,
		0x8000000000000000ULL, 0xffffffffffffffffULL,
	};
	constexpr std::array<std::uint64_t, 2> entry_flags{0x2, 0xfff};

	for (const auto operation : operations)
		for (const auto width : widths)
			for (const auto left : values)
				for (const auto right : values)
					for (const std::uint8_t carry : {0, 1})
						/* Only ADC and SBB record an incoming carry; the
						 * recording macro zeroes the byte for every other
						 * operation, so a carry there is not a reachable state.
						 */
						if (carry == 0 ||
							operation == DeferredFlagOperation::add_carry ||
							operation ==
								DeferredFlagOperation::subtract_borrow)
						for (const auto entry : entry_flags)
							for (const int sense : {0, 1}) {
		CpuState cpu{};
		cpu.rip = 0;
		cpu.rflags = entry;
		cpu.gpr(Register::rsp) = stack;
		cpu.deferred_flags.left = left;
		cpu.deferred_flags.right = right;
		cpu.deferred_flags.result =
			sense == 0 ? left + right + carry : left - right - carry;
		cpu.deferred_flags.operation = operation;
		cpu.deferred_flags.width = width;
		cpu.deferred_flags.carry = carry;

		CpuState reference = cpu;
		materialize_flags(reference);

		const auto run = gadgets::execute(block.program, cpu, memory, 1);
		if (!expect(run.event == uml::cpu::EngineEvent::continued &&
			run.retired_instructions == 1,
			"PUSHFQ did not retire"))
			return false;
		std::uint64_t pushed = 0;
		std::byte raw[sizeof(pushed)]{};
		if (!expect(memory.read(stack - sizeof(pushed),
				std::span<std::byte>{raw, sizeof(raw)}),
			"PUSHFQ destination was not readable"))
			return false;
		std::memcpy(&pushed, raw, sizeof(pushed));
		if (!expect(pushed == reference.rflags &&
			cpu.rflags == reference.rflags,
			"materialized flags disagree with the reference"))
			return false;
	}
	return true;
}

[[nodiscard]] bool carry_branches_match_the_reference()
{
	using namespace uml::cpu::x86_64;
	/* The four carry-based conditions read CF and ZF straight out of the
	 * deferred record. Every recorded operation and width has to reach the
	 * same decision the materialized flags would.
	 */
	TestMemory memory;
	struct Branch final {
		std::uint64_t address;
		std::uint8_t opcode;
		const char *name;
	};
	constexpr std::array<Branch, 4> branches{{
		{0, 0x72, "JB"}, {16, 0x73, "JAE"}, {32, 0x76, "JBE"}, {48, 0x77, "JA"},
	}};
	constexpr std::array<DeferredFlagOperation, 8> operations{
		DeferredFlagOperation::none,
		DeferredFlagOperation::add,
		DeferredFlagOperation::subtract,
		DeferredFlagOperation::logical,
		DeferredFlagOperation::increment,
		DeferredFlagOperation::decrement,
		DeferredFlagOperation::add_carry,
		DeferredFlagOperation::subtract_borrow,
	};
	constexpr std::array<std::uint8_t, 4> widths{1, 2, 4, 8};
	constexpr std::array<std::uint64_t, 6> values{
		0, 1, 0x80, 0xff, 0x8000000000000000ULL, 0xffffffffffffffffULL,
	};

	for (const auto branch : branches) {
		memory.load(branch.address, {branch.opcode, 0x05});
		const auto built = gadgets::build(memory, branch.address);
		if (!expect(built.status == gadgets::BuildStatus::built,
			"carry-condition branch did not build"))
			return false;

		for (const auto operation : operations)
			for (const auto width : widths)
				for (const auto left : values)
					for (const auto right : values)
						for (const std::uint16_t override_mask : {0, 1}) {
			CpuState cpu{};
			cpu.rip = branch.address;
			cpu.rflags = 0x2;
			cpu.deferred_flags.left = left;
			cpu.deferred_flags.right = right;
			cpu.deferred_flags.result = left - right;
			cpu.deferred_flags.operation = operation;
			cpu.deferred_flags.width = width;
			cpu.deferred_flags.override_mask = override_mask;
			cpu.deferred_flags.override_value = override_mask;

			CpuState reference = cpu;
			materialize_flags(reference);
			const bool carry = (reference.rflags & 0x1) != 0;
			const bool zero = (reference.rflags & 0x40) != 0;
			const bool taken =
				branch.opcode == 0x72 ? carry :
				branch.opcode == 0x73 ? !carry :
				branch.opcode == 0x76 ? (carry || zero) : !(carry || zero);
			const auto expected =
				branch.address + (taken ? 7 : 2);

			const auto run = gadgets::execute(built.program, cpu, memory, 1);
			if (!expect(run.event == uml::cpu::EngineEvent::continued &&
					run.retired_instructions == 1 && cpu.rip == expected,
				"a carry-condition branch disagrees with the reference"))
				return false;
		}
	}
	return true;
}

[[nodiscard]] bool execute_compat32_test_greater_after_syscall()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x85, 0xc0, // test eax, eax
		0x7f, 0x05, // jg 9
	});
	const auto block = gadgets::build(memory, 0, ExecutionMode::compat32, 2);
	if (!expect(block.status == gadgets::BuildStatus::built,
		"compat32 TEST/JG block did not build"))
		return false;

	const auto run = [&](std::uint64_t value, std::uint64_t expected_rip,
		const char *message) {
		CpuState cpu{};
		cpu.execution_mode = ExecutionMode::compat32;
		cpu.gpr(Register::rax) = value;
		const auto result = gadgets::execute(block.program, cpu, memory, 2);
		return expect(result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 2 && cpu.rip == expected_rip,
			message);
	};

	return run(0, 4, "compat32 TEST/JG treated zero as positive") &&
		run(1, 9, "compat32 TEST/JG rejected a positive result") &&
		run(0xffff'ffff'ffff'fff5ULL, 4,
			"compat32 TEST/JG treated sign-extended -EAGAIN as positive") &&
		run(0x1234'5678'ffff'fff5ULL, 4,
			"compat32 TEST/JG used bits above EAX for the sign test");
}

/* Match glibc's complete i386 futex-result validator.  Its accepted common
 * results pass through signed comparisons and a register-indexed BT before
 * the rare timeout/overflow cases take equality branches.
 */
[[nodiscard]] bool execute_compat32_futex_result_validator()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x85, 0xc0,                         // test eax, eax
		0x7f, 0x23,                         // jg fatal
		0x83, 0xf8, 0xea,                   // cmp eax, -22
		0x7c, 0x14,                         // jl special
		0x8d, 0x50, 0x16,                   // lea edx, [eax + 22]
		0xb9, 0x01, 0x08, 0x44, 0x00,       // mov ecx, 0x440801
		0x0f, 0xa3, 0xd1,                   // bt ecx, edx
		0x73, 0x11,                         // jae fatal
		0xbf, 0x2a, 0x00, 0x00, 0x00,       // success: mov edi, 42
		0xcd, 0x80,                         // int 0x80
		0x83, 0xf8, 0x92,                   // special: cmp eax, -110
		0x74, 0xf4,                         // je success
		0x83, 0xf8, 0xb5,                   // cmp eax, -75
		0x74, 0xef,                         // je success
		0xbf, 0x63, 0x00, 0x00, 0x00,       // fatal: mov edi, 99
		0xcd, 0x80,                         // int 0x80
	});
	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);

	const auto run = [&](std::uint32_t value, std::uint64_t marker,
		const char *message) {
		CpuState cpu{};
		cpu.execution_mode = ExecutionMode::compat32;
		cpu.gpr(Register::rax) = value;
		for (unsigned step = 0; step < 8; ++step) {
			const auto block = executor.select_program(
				memory, cpu.rip, ExecutionMode::compat32, 64);
			if (block.status != gadgets::BuildStatus::built ||
				block.program == nullptr)
				return expect(false, "compat32 futex validator did not build");
			const auto result = gadgets::execute(
				*block.program, cpu, memory, executor, 64);
			if (result.event == uml::cpu::EngineEvent::system_call)
				return expect(cpu.gpr(Register::rdi) == marker, message);
			if (result.event != uml::cpu::EngineEvent::continued)
				return expect(false, "compat32 futex validator exited early");
		}
		return expect(false, "compat32 futex validator did not terminate");
	};

	const auto run_all = [&] {
		return run(0, 42, "futex validator rejected success") &&
		run(static_cast<std::uint32_t>(-4), 42,
			"futex validator rejected EINTR") &&
		run(static_cast<std::uint32_t>(-11), 42,
			"futex validator rejected EAGAIN") &&
		run(static_cast<std::uint32_t>(-22), 42,
			"futex validator rejected EINVAL") &&
		run(static_cast<std::uint32_t>(-110), 42,
			"futex validator rejected ETIMEDOUT") &&
		run(static_cast<std::uint32_t>(-75), 42,
			"futex validator rejected EOVERFLOW") &&
		run(1, 99, "futex validator accepted a positive result") &&
		run(static_cast<std::uint32_t>(-516), 99,
			"futex validator accepted an internal restart result");
	};
	return run_all() && run_all();
}

[[nodiscard]] bool shifts_overlay_lazy_arithmetic_flags()
{
	using namespace uml::cpu::x86_64;
	TestMemory rotate_memory;
	rotate_memory.load(0, {
		0x01, 0xd8, // add eax, ebx
		0xd1, 0xc8, // ror eax, 1
	});
	const auto rotate = gadgets::build(rotate_memory, 0, 2);
	if (!expect(rotate.status == gadgets::BuildStatus::built,
		"lazy ROR block did not build"))
		return false;
	CpuState cpu{};
	cpu.gpr(Register::rbx) = 1;
	const auto rotate_result = gadgets::execute(
		rotate.program, cpu, rotate_memory, 2);
	constexpr auto rotate_mask = rflags_carry | rflags_overflow;
	if (!expect(rotate_result.event == uml::cpu::EngineEvent::continued &&
			rotate_result.retired_instructions == 2 &&
			cpu.gpr(Register::rax) == 0x8000'0000,
		"lazy ROR produced the wrong value") ||
		!expect(cpu.deferred_flags.operation == DeferredFlagOperation::add &&
			cpu.deferred_flags.override_mask == rotate_mask &&
			cpu.deferred_flags.override_value == rotate_mask,
			"ROR eagerly materialized or lost its flag overrides"))
		return false;
	materialize_flags(cpu);
	if (!expect((cpu.rflags & rotate_mask) == rotate_mask &&
			(cpu.rflags & (rflags_zero | rflags_sign | rflags_parity)) == 0,
		"materialized ROR flags did not preserve the preceding ADD flags") ||
		!expect(cpu.deferred_flags.operation == DeferredFlagOperation::none &&
			cpu.deferred_flags.override_mask == 0,
			"materialization did not consume the shift overlay"))
		return false;

	TestMemory branch_memory;
	branch_memory.load(0, {
		0x01, 0xd8,       // add eax, ebx
		0xc1, 0xe8, 0x1f, // shr eax, 31
		0x74, 0x05,       // je +5
	});
	const auto branch = gadgets::build(branch_memory, 0);
	if (!expect(branch.status == gadgets::BuildStatus::built,
		"lazy shift/branch block did not build"))
		return false;
	cpu = CpuState{};
	cpu.gpr(Register::rax) = 1;
	cpu.gpr(Register::rbx) = 1;
	const auto branch_result = gadgets::execute(
		branch.program, cpu, branch_memory, 3);
	/* SHL, SHR and SAR record their operands instead of overlaying: the
	 * zero branch reads the recorded result and leaves the record pending. */
	return expect(branch_result.event == uml::cpu::EngineEvent::continued &&
			branch_result.retired_instructions == 3 && cpu.rip == 12,
		"JE did not consume the deferred shift result") &&
		expect(cpu.deferred_flags.operation ==
				DeferredFlagOperation::shift_right &&
			cpu.deferred_flags.result == 0 &&
			cpu.deferred_flags.override_mask == 0,
			"zero-branch fast path materialized the deferred shift");
}

/* The deferred shift record is materialized by the engine (through the full
 * and the carry-only routines) and by the C++ materializer; check all three
 * against the architectural definition for every width, a spread of counts
 * including ones at and beyond the width, and both shift directions. The
 * auxiliary flag is undefined and overflow is only defined for a count of
 * one, so those are not compared elsewhere.
 */
[[nodiscard]] bool lazy_shift_flags_match_the_definition()
{
	using namespace uml::cpu::x86_64;
	static constexpr std::array<std::uint64_t, 9> values{
		0U, 1U, 2U, 0x81U, 0x7fffU, 0x8001U, 0x4000'0001ULL,
		0x8000'0000'0000'0001ULL, 0xc5a3'ffff'0000'a5c3ULL};
	for (unsigned kind = 0; kind < 3U; ++kind) {
		for (const unsigned width : {8U, 16U, 32U, 64U}) {
			const unsigned bits = width;
			const auto mask = bits == 64U ? ~std::uint64_t{} :
				(std::uint64_t{1} << bits) - 1U;
			const auto sign = std::uint64_t{1} << (bits - 1U);
			for (const unsigned count : {1U, 3U, bits - 1U, bits, bits + 3U,
					bits == 64U ? 63U : 31U}) {
				if (count > (bits == 64U ? 63U : 31U))
					continue;
				for (const auto value : values) {
					for (const bool then_inc : {false, true}) {
						std::vector<std::byte> code;
						const auto push = [&code](unsigned byte) {
							code.push_back(static_cast<std::byte>(byte));
						};
						if (width == 16U)
							push(0x66);
						if (width == 64U)
							push(0x48);
						push(width == 8U ? 0xc0 : 0xc1);
						push(kind == 0 ? 0xe0 : kind == 1 ? 0xe8 : 0xf8);
						push(count);
						if (then_inc) {
							push(0xff); // inc ebx
							push(0xc3);
						}
						push(0x0f);
						push(0x05);
						TestMemory memory;
						memory.fill(0, code);
						const auto built = gadgets::build(memory, 0);
						if (!expect(built.status == gadgets::BuildStatus::built,
							"shift flag program did not build"))
							return false;
						CpuState cpu{};
						cpu.gpr(Register::rax) = value;
						const auto result = gadgets::execute(
							built.program, cpu, memory, 8);
						if (!expect(result.event ==
							uml::cpu::EngineEvent::system_call,
							"shift flag program did not reach SYSCALL"))
							return false;
						materialize_flags(cpu);
						const auto v = value & mask;
						std::uint64_t expected_result;
						bool carry;
						if (kind == 0) {
							expected_result = (v << count) & mask;
							carry = count <= bits &&
								((v >> (bits - count)) & 1U) != 0;
						} else if (kind == 1) {
							expected_result = v >> count;
							carry = count <= bits &&
								((v >> (count - 1U)) & 1U) != 0;
						} else {
							const auto extended = (v & sign) != 0 ?
								v | ~mask : v;
							expected_result = static_cast<std::uint64_t>(
								static_cast<std::int64_t>(extended) >>
									count) & mask;
							carry = ((v >> (count <= bits ? count - 1U :
								bits - 1U)) & 1U) != 0;
						}
						std::uint64_t expected = 0;
						if (carry)
							expected |= rflags_carry;
						if (expected_result == 0)
							expected |= rflags_zero;
						if ((expected_result & sign) != 0)
							expected |= rflags_sign;
						unsigned ones = 0;
						for (unsigned bit = 0; bit < 8U; ++bit)
							ones += (expected_result >> bit) & 1U;
						if (ones % 2U == 0)
							expected |= rflags_parity;
						auto compared = rflags_carry | rflags_zero |
							rflags_sign | rflags_parity;
						if (count == 1U && !then_inc) {
							compared |= rflags_overflow;
							if (kind == 0 && ((((expected_result & sign) != 0)
									? 1U : 0U) ^ (carry ? 1U : 0U)) != 0)
								expected |= rflags_overflow;
							if (kind == 1 && (v & sign) != 0)
								expected |= rflags_overflow;
						}
						/* INC keeps only the shift's carry; its own record
						 * defines the rest. */
						if (then_inc)
							compared = rflags_carry;
						const auto observed = cpu.rflags & compared;
						if (observed != (expected & compared) ||
							(cpu.gpr(Register::rax) & mask) != expected_result) {
							char message[200];
							std::snprintf(message, sizeof(message),
								"lazy %s width %u count %u value %#llx%s: "
								"flags %#llx expected %#llx, result %#llx",
								kind == 0 ? "SHL" : kind == 1 ? "SHR" : "SAR",
								width, count,
								static_cast<unsigned long long>(value),
								then_inc ? " then INC" : "",
								static_cast<unsigned long long>(observed),
								static_cast<unsigned long long>(
									expected & compared),
								static_cast<unsigned long long>(
									cpu.gpr(Register::rax) & mask));
							return expect(false, message);
						}
					}
				}
			}
		}
	}
	return true;
}

[[nodiscard]] bool execute_segment_memory()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x64, 0x48, 0x89, 0x43, 0x08,
		0x64, 0x48, 0x8b, 0x4b, 0x08,
	});
	CpuState cpu{};
	cpu.fs_base = 64;
	cpu.gpr(Register::rbx) = 16;
	cpu.gpr(Register::rax) = 0x1122'3344'5566'7788ULL;
	const auto built = gadgets::build(memory, 0, 2);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"FS memory block did not build"))
		return false;
	const auto result = gadgets::execute(built.program, cpu, memory, 2);
	return expect(result.event == uml::cpu::EngineEvent::continued,
		"FS memory block returned the wrong event") &&
		expect(result.retired_instructions == 2,
			"FS memory retirement is wrong") &&
		expect(memory.qword(88) == 0x1122'3344'5566'7788ULL &&
			cpu.gpr(Register::rcx) == 0x1122'3344'5566'7788ULL,
			"FS base was not applied to memory operands");
}

/* Address generation resolves base, index, scale, segment and address size
 * from one stream descriptor with no branch on which of them are present, so
 * every shape the builder can encode needs its own case. The instruction
 * corpus is keyed by operand form rather than addressing shape and cannot
 * cover this.
 */
[[nodiscard]] bool execute_addressing_shapes()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t data = 0x800;
	constexpr std::uint64_t marker = 0x0123'4567'89ab'cdefULL;

	struct Shape final {
		const char *name;
		std::initializer_list<std::uint8_t> encoding;
		std::uint64_t rbx;
		std::uint64_t rcx;
		std::uint64_t fs;
		std::uint64_t gs;
	};
	/* Every shape below resolves to the same effective address, so a wrong
	 * result names the shape that mis-resolved.
	 */
	const std::initializer_list<Shape> shapes{
		{"(base)", {0x48, 0x8b, 0x03}, data, 0, 0, 0},
		{"disp(base)", {0x48, 0x8b, 0x43, 0x10}, data - 16, 0, 0, 0},
		{"(base,index,8)", {0x48, 0x8b, 0x04, 0xcb}, 0x400, 0x80, 0, 0},
		{"disp(base,index,8)", {0x48, 0x8b, 0x44, 0xcb, 0x10},
			0x3f0, 0x80, 0, 0},
		{"(,index,8)", {0x48, 0x8b, 0x04, 0xcd, 0x00, 0x00, 0x00, 0x00},
			0, 0x100, 0, 0},
		{"fs:disp", {0x64, 0x48, 0x8b, 0x04, 0x25, 0x10, 0x00, 0x00, 0x00},
			0, 0, data - 16, 0},
		{"gs:disp", {0x65, 0x48, 0x8b, 0x04, 0x25, 0x10, 0x00, 0x00, 0x00},
			0, 0, 0, data - 16},
		{"absolute", {0x48, 0x8b, 0x04, 0x25, 0x00, 0x08, 0x00, 0x00},
			0, 0, 0, 0},
		{"addr32 disp(base)", {0x67, 0x48, 0x8b, 0x43, 0x10},
			data - 16, 0, 0, 0},
		{"addr32 (base,index,8)", {0x67, 0x48, 0x8b, 0x04, 0xcb},
			0x400, 0x80, 0, 0},
	};

	std::uint64_t code = 0x100;
	for (const auto &shape : shapes) {
		TestMemory memory;
		memory.load(data, {0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01});
		memory.load(code, shape.encoding);
		CpuState cpu{};
		cpu.rip = code;
		cpu.gpr(Register::rbx) = shape.rbx;
		cpu.gpr(Register::rcx) = shape.rcx;
		cpu.fs_base = shape.fs;
		cpu.gs_base = shape.gs;
		const auto built = gadgets::build(memory, code, 1);
		if (!expect(built.status == gadgets::BuildStatus::built, shape.name) ||
			!expect(gadgets::execute(built.program, cpu, memory, 1).event ==
				uml::cpu::EngineEvent::continued, shape.name) ||
			!expect(cpu.gpr(Register::rax) == marker, shape.name))
			return false;
		code += 16;
	}

	/* RIP-relative folds the displacement against the instruction end, so it
	 * must be built where it is actually placed.
	 */
	{
		TestMemory memory;
		memory.load(data, {0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01});
		const std::uint64_t rip_code = 0x200;
		const auto displacement = static_cast<std::uint32_t>(
			data - (rip_code + 7));
		memory.load(rip_code, {0x48, 0x8b, 0x05,
			static_cast<std::uint8_t>(displacement),
			static_cast<std::uint8_t>(displacement >> 8),
			static_cast<std::uint8_t>(displacement >> 16),
			static_cast<std::uint8_t>(displacement >> 24)});
		CpuState cpu{};
		cpu.rip = rip_code;
		const auto built = gadgets::build(memory, rip_code, 1);
		if (!expect(built.status == gadgets::BuildStatus::built,
				"rip-relative did not build") ||
			!expect(gadgets::execute(built.program, cpu, memory, 1).event ==
				uml::cpu::EngineEvent::continued,
				"rip-relative did not execute") ||
			!expect(cpu.gpr(Register::rax) == marker,
				"rip-relative resolved the wrong address"))
			return false;
	}

	/* A load that spans a page boundary leaves the inline fast path, so the
	 * shape has to stay correct on the slow path too.
	 */
	{
		TestMemory memory;
		memory.load(0xffc, {0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01});
		memory.load(0x100, {0x48, 0x8b, 0x43, 0x10});
		CpuState cpu{};
		cpu.rip = 0x100;
		cpu.gpr(Register::rbx) = 0xffc - 16;
		const auto built = gadgets::build(memory, 0x100, 1);
		if (!expect(built.status == gadgets::BuildStatus::built,
				"page-crossing load did not build") ||
			!expect(gadgets::execute(built.program, cpu, memory, 1).event ==
				uml::cpu::EngineEvent::continued,
				"page-crossing load did not execute") ||
			!expect(cpu.gpr(Register::rax) == marker,
				"page-crossing load resolved the wrong address"))
			return false;
	}

	/* The always-zero slot the absent-register encoding names must stay zero:
	 * a stray write to it would silently offset every such operand.
	 */
	CpuState fresh{};
	return expect(fresh.segment_zero == 0,
		"the address-generation zero slot is not zero");
}

/* PMOVMSKB collapses one sign bit per byte into a bit mask. The corpus carries
 * only one or two cases per width, which a lane-ordering mistake can survive,
 * so walk a single set bit through every position instead.
 */
[[nodiscard]] bool execute_byte_sign_masks()
{
	using namespace uml::cpu::x86_64;
	const auto run = [](std::initializer_list<std::uint8_t> encoding,
		std::size_t lanes, std::size_t set_lane) -> std::uint64_t {
		TestMemory memory;
		memory.load(0, encoding);
		CpuState cpu{};
		for (std::size_t lane = 0; lane < lanes; ++lane)
			cpu.vectors[1][lane] = static_cast<std::byte>(
				lane == set_lane ? 0x80 : 0x7f);
		const auto built = gadgets::build(memory, 0, 1);
		if (built.status != gadgets::BuildStatus::built)
			return ~std::uint64_t{0};
		if (gadgets::execute(built.program, cpu, memory, 1).event !=
			uml::cpu::EngineEvent::continued)
			return ~std::uint64_t{0};
		return cpu.gpr(Register::rax);
	};

	for (std::size_t lane = 0; lane < 16; ++lane)
		if (!expect(run({0x66, 0x0f, 0xd7, 0xc1}, 16, lane) ==
			(std::uint64_t{1} << lane), "PMOVMSKB xmm lane"))
			return false;
	for (std::size_t lane = 0; lane < 32; ++lane)
		if (!expect(run({0xc5, 0xfd, 0xd7, 0xc1}, 32, lane) ==
			(std::uint64_t{1} << lane), "VPMOVMSKB ymm lane"))
			return false;

	/* All sign bits set, and none, bound the walk at both ends. */
	TestMemory memory;
	memory.load(0, {0x66, 0x0f, 0xd7, 0xc1});
	CpuState cpu{};
	for (auto &lane : cpu.vectors[1])
		lane = std::byte{0xff};
	const auto built = gadgets::build(memory, 0, 1);
	if (!expect(built.status == gadgets::BuildStatus::built,
			"PMOVMSKB saturated case did not build") ||
		!expect(gadgets::execute(built.program, cpu, memory, 1).event ==
			uml::cpu::EngineEvent::continued,
			"PMOVMSKB saturated case did not execute"))
		return false;
	return expect(cpu.gpr(Register::rax) == 0xffff,
		"PMOVMSKB did not set every mask bit");
}

[[nodiscard]] bool execute_call_and_return()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xe8, 0x05, 0x00, 0x00, 0x00});
	memory.load(10, {0xc3});
	CpuState cpu{};
	cpu.gpr(Register::rsp) = 400;
	const auto caller = gadgets::build(memory, 0);
	if (!expect(caller.status == gadgets::BuildStatus::built,
		"CALL did not build"))
		return false;
	const auto call_result = gadgets::execute(caller.program, cpu, memory, 1);
	if (!expect(call_result.event == uml::cpu::EngineEvent::continued &&
		cpu.rip == 10 && cpu.gpr(Register::rsp) == 392 &&
		memory.qword(392) == 5,
		"CALL did not push its return address"))
		return false;
	const auto callee = gadgets::build(memory, cpu.rip);
	if (!expect(callee.status == gadgets::BuildStatus::built,
		"RET did not build"))
		return false;
	const auto return_result = gadgets::execute(
		callee.program, cpu, memory, 1);
	return expect(return_result.event == uml::cpu::EngineEvent::continued &&
		cpu.rip == 5 && cpu.gpr(Register::rsp) == 400,
		"RET did not restore RIP/RSP");
}

[[nodiscard]] bool decode_compat32_defaults()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x40,                         // inc eax, not REX
		0x8b, 0x05, 0x00, 0x01, 0x00, 0x00, // mov eax, [0x100]
	});
	const auto increment = decode(memory, 0, ExecutionMode::compat32);
	if (!expect(increment.status == DecodeStatus::decoded &&
		increment.instruction.operation == Operation::increment &&
		increment.instruction.destination.width == OperandWidth::dword &&
		increment.instruction.destination.reg.index == 0 &&
		increment.instruction.length == 1,
		"compat32 opcode 40 was not decoded as INC EAX"))
		return false;
	const auto load = decode(memory, 1, ExecutionMode::compat32);
	const auto &source = load.instruction.source;
	return expect(load.status == DecodeStatus::decoded &&
		load.instruction.destination.width == OperandWidth::dword,
		"compat32 ordinary operand width is not 32-bit") &&
		expect(source.kind == OperandKind::memory &&
			source.memory.address_32 && source.memory.absolute_address &&
			source.memory.absolute == 0x100 && !source.memory.rip_relative,
			"compat32 ModR/M rm=5 is not an absolute 32-bit address");
}

[[nodiscard]] bool execute_compat32_stack_and_int80()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x53,             // push ebx
		0x59,             // pop ecx
		0xcd, 0x80,       // int 80h
	});
	CpuState cpu{};
	cpu.execution_mode = ExecutionMode::compat32;
	cpu.gpr(Register::rsp) = 0x1'0000'0200ULL;
	cpu.gpr(Register::rbx) = 0xaaaa'bbbb'1122'3344ULL;
	const auto built = gadgets::build(memory, 0, ExecutionMode::compat32);
	if (!expect(built.status == gadgets::BuildStatus::built &&
		built.program.execution_mode() == ExecutionMode::compat32,
		"compat32 stack/syscall block did not build with mode identity"))
		return false;
	const auto result = gadgets::execute(built.program, cpu, memory, 3);
	return expect(result.event == uml::cpu::EngineEvent::system_call &&
		result.retired_instructions == 3,
		"compat32 INT 80h did not exit after the stack operations") &&
		expect(cpu.gpr(Register::rsp) == 0x200 &&
			cpu.gpr(Register::rcx) == 0x1122'3344 &&
			memory.dword(0x1fc) == 0x1122'3344,
			"compat32 PUSH/POP did not use a wrapping 32-bit stack") &&
		expect(cpu.system_call_32bit && cpu.rip == 4,
			"compat32 INT 80h did not publish its syscall convention or EIP");
}

[[nodiscard]] bool execute_compat32_pushfd_popfd()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x9c,                         // pushfd
		0x58,                         // pop eax
		0x35, 0x00, 0x00, 0x20, 0x00, // xor eax, 0x200000 (toggle ID flag)
		0x50,                         // push eax
		0x9d,                         // popfd
		0x9c,                         // pushfd
		0x5b,                         // pop ebx
	});
	CpuState cpu{};
	cpu.execution_mode = ExecutionMode::compat32;
	cpu.gpr(Register::rsp) = 0x200;
	cpu.rflags = 0x2;
	const auto built = gadgets::build(memory, 0, ExecutionMode::compat32);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"compat32 pushfd/popfd block did not build"))
		return false;
	const auto result = gadgets::execute(built.program, cpu, memory, 7);
	if (!expect(result.event == uml::cpu::EngineEvent::continued &&
		result.retired_instructions == 7,
		"compat32 pushfd/popfd block did not retire all instructions"))
		return false;
	return expect((cpu.rflags & 0x200000ULL) != 0,
		"compat32 popfd did not set the ID flag in EFLAGS") &&
		expect((cpu.gpr(Register::rbx) & 0x200000ULL) != 0,
		"compat32 pushfd did not observe the updated ID flag");
}


[[nodiscard]] bool execute_compat32_legacy_segment_stack()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x1f, // pop ds -- first instruction in Wine's context-return tail
		0xcf, // iret
	});
	memory.load(0x200, {0x2b, 0x00, 0x00, 0x00});
	CpuState cpu{};
	cpu.execution_mode = ExecutionMode::compat32;
	cpu.gpr(Register::rsp) = 0x1'0000'0200ULL;
	const auto pop_ds = gadgets::build(
		memory, 0, ExecutionMode::compat32, 1);
	if (!expect(pop_ds.status == gadgets::BuildStatus::built,
		"compat32 POP DS did not build"))
		return false;
	const auto popped = gadgets::execute(pop_ds.program, cpu, memory, 1);
	if (!expect(popped.event == uml::cpu::EngineEvent::continued &&
			popped.retired_instructions == 1 && cpu.rip == 1,
		"compat32 POP DS did not retire before IRET") ||
		!expect(cpu.segment_selectors[3] == 0x2b &&
			cpu.gpr(Register::rsp) == 0x204,
			"compat32 POP DS did not load a 32-bit stack slot"))
		return false;
	const auto iret = gadgets::build(
		memory, cpu.rip, ExecutionMode::compat32, 1);
	if (!expect(iret.status == gadgets::BuildStatus::built,
		"compat32 IRET after POP DS did not build"))
		return false;

	TestMemory round_trip_memory;
	round_trip_memory.load(0, {
		0x1e, // push ds
		0x07, // pop es
	});
	cpu = CpuState{};
	cpu.execution_mode = ExecutionMode::compat32;
	cpu.gpr(Register::rsp) = 0x204;
	cpu.segment_selectors[3] = 0x33;
	const auto round_trip = gadgets::build(
		round_trip_memory, 0, ExecutionMode::compat32, 2);
	if (!expect(round_trip.status == gadgets::BuildStatus::built,
		"compat32 segment PUSH/POP pair did not build"))
		return false;
	const auto transferred = gadgets::execute(
		round_trip.program, cpu, round_trip_memory, 2);
	if (!expect(transferred.event == uml::cpu::EngineEvent::continued &&
			transferred.retired_instructions == 2 && cpu.rip == 2,
		"compat32 segment PUSH/POP pair did not retire") ||
		!expect(cpu.segment_selectors[0] == 0x33 &&
			cpu.gpr(Register::rsp) == 0x204 &&
			round_trip_memory.dword(0x200) == 0x33,
			"compat32 segment PUSH/POP pair lost its selector"))
		return false;

	TestMemory opcode_memory;
	opcode_memory.load(0, {0x06, 0x07, 0x0e, 0x16, 0x17, 0x1e, 0x1f});
	constexpr std::array<std::uint8_t, 7> segments{0, 0, 1, 2, 2, 3, 3};
	constexpr std::array<std::uint8_t, 7> pushes{1, 0, 1, 1, 0, 1, 0};
	for (std::size_t index = 0; index < segments.size(); ++index) {
		const auto decoded = decode(
			opcode_memory, index, ExecutionMode::compat32);
		if (!expect(decoded.status == DecodeStatus::decoded &&
				decoded.instruction.operation == Operation::segment_stack &&
				decoded.instruction.vector_group == segments[index] &&
				decoded.instruction.vector_control == pushes[index],
			"legacy i386 segment-stack opcode did not decode"))
			return false;
	}
	return expect(decode(opcode_memory, 0, ExecutionMode::long64).status ==
		DecodeStatus::illegal_instruction,
		"long64 mode accepted a legacy segment-stack opcode");
}

[[nodiscard]] bool execute_long64_interrupt_return_stack_switch()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xcf}); // iretq
	constexpr std::uint64_t stack = 0x200;
	constexpr std::uint64_t restored_stack = 0x900;
	constexpr std::uint64_t restored_rip = 0x1234'5678;
	const std::array<std::uint64_t, 5> frame{
		restored_rip,
		0x33,
		0x246,
		restored_stack,
		0x2b,
	};
	if (!memory.write(stack, std::as_bytes(std::span{frame})))
		return expect(false, "long64 IRETQ frame setup failed");
	const auto built = gadgets::build(
		memory, 0, ExecutionMode::long64, 1);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"long64 IRETQ did not build"))
		return false;
	CpuState cpu{};
	cpu.rip = 0;
	cpu.gpr(Register::rsp) = stack;
	cpu.segment_selectors[1] = 0x33;
	cpu.segment_selectors[2] = 0x2b;
	const auto result = gadgets::execute(built.program, cpu, memory, 1);
	return expect(result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 1,
			"long64 IRETQ did not retire") &&
		expect(cpu.rip == restored_rip &&
			cpu.gpr(Register::rsp) == restored_stack,
			"long64 IRETQ left the restored RSP on its old stack") &&
		expect(cpu.segment_selectors[1] == 0x33 &&
			cpu.segment_selectors[2] == 0x2b && cpu.rflags == 0x246,
			"long64 IRETQ did not restore its selectors and flags");
}

[[nodiscard]] bool expose_consistent_xsave_cpuid_state()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x0f, 0xa2});          // cpuid
	memory.load(16, {0x0f, 0x01, 0xd0}); // xgetbv
	const auto cpuid = gadgets::build(memory, 0, ExecutionMode::long64, 1);
	const auto xgetbv = gadgets::build(memory, 16, ExecutionMode::long64, 1);
	if (!expect(cpuid.status == gadgets::BuildStatus::built &&
			xgetbv.status == gadgets::BuildStatus::built,
		"extended-state identification instructions did not build"))
		return false;

	const auto run_cpuid = [&](std::uint32_t leaf, std::uint32_t subleaf,
			CpuState &cpu) {
		cpu = CpuState{};
		cpu.gpr(Register::rax) = leaf;
		cpu.gpr(Register::rcx) = subleaf;
		return gadgets::execute(cpuid.program, cpu, memory, 1);
	};
	CpuState cpu{};
	auto result = run_cpuid(0, 0, cpu);
	if (!expect(result.event == uml::cpu::EngineEvent::continued &&
			cpu.gpr(Register::rax) == 13,
		"CPUID omitted its XSAVE enumeration leaf"))
		return false;
	result = run_cpuid(1, 0, cpu);
	constexpr std::uint64_t through_avx2_leaf_one =
		(1ULL << 0) | (1ULL << 1) | (1ULL << 9) | (1ULL << 12) |
		(1ULL << 13) | (1ULL << 19) | (1ULL << 20) | (1ULL << 22) |
		(1ULL << 23) | (1ULL << 25) | (1ULL << 26) | (1ULL << 27) |
		(1ULL << 28) | (1ULL << 29) | (1ULL << 30);
	if (!expect(result.event == uml::cpu::EngineEvent::continued &&
			(cpu.gpr(Register::rcx) & through_avx2_leaf_one) ==
				through_avx2_leaf_one,
		"CPUID leaf 1 omitted a qualified through-AVX2 feature"))
		return false;
	result = run_cpuid(7, 0, cpu);
	constexpr std::uint64_t through_avx2_leaf_seven_ebx =
		(1ULL << 0) | (1ULL << 3) | (1ULL << 5) | (1ULL << 8) |
		(1ULL << 9) | (1ULL << 18) | (1ULL << 19) | (1ULL << 23) |
		(1ULL << 24) | (1ULL << 29);
	if (!expect(result.event == uml::cpu::EngineEvent::continued &&
			(cpu.gpr(Register::rbx) & through_avx2_leaf_seven_ebx) ==
				through_avx2_leaf_seven_ebx &&
			(cpu.gpr(Register::rcx) & (1ULL << 22)) != 0,
		"CPUID leaf 7 omitted a qualified AVX2-era extension"))
		return false;
	result = run_cpuid(13, 0, cpu);
	if (!expect(result.event == uml::cpu::EngineEvent::continued &&
			cpu.gpr(Register::rax) == 7 &&
			cpu.gpr(Register::rbx) == 832 &&
			cpu.gpr(Register::rcx) == 832 &&
			cpu.gpr(Register::rdx) == 0,
		"CPUID leaf 0xD did not describe the complete XSAVE image"))
		return false;
	result = run_cpuid(13, 2, cpu);
	if (!expect(result.event == uml::cpu::EngineEvent::continued &&
			cpu.gpr(Register::rax) == 256 &&
			cpu.gpr(Register::rbx) == 576 &&
			cpu.gpr(Register::rcx) == 0 &&
			cpu.gpr(Register::rdx) == 0,
		"CPUID leaf 0xD subleaf 2 did not locate AVX state"))
		return false;

	cpu = CpuState{};
	cpu.rip = 16;
	cpu.gpr(Register::rcx) = 0;
	result = gadgets::execute(xgetbv.program, cpu, memory, 1);
	return expect(result.event == uml::cpu::EngineEvent::continued &&
			cpu.gpr(Register::rax) == 7 &&
			cpu.gpr(Register::rdx) == 0,
		"XGETBV did not agree with the CPUID XSAVE feature mask");
}

[[nodiscard]] bool wrap_compat32_instruction_pointer()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t base = 0xffff'f000ULL;
	constexpr std::uint64_t address = 0xffff'fff0ULL;
	TestMemory memory{base};
	memory.load(address, {0xe9, 0x30, 0x00, 0x00, 0x00});
	CpuState cpu{};
	cpu.execution_mode = ExecutionMode::compat32;
	cpu.rip = address;
	const auto built = gadgets::build(
		memory, address, ExecutionMode::compat32, 1);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"compat32 wrapping JMP did not build"))
		return false;
	const auto result = gadgets::execute(built.program, cpu, memory, 1);
	return expect(result.event == uml::cpu::EngineEvent::continued &&
		result.retired_instructions == 1 && cpu.rip == 0x25,
		"compat32 relative branch did not wrap EIP at 32 bits");
}

[[nodiscard]] bool execute_compat32_tls_segment()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x33, 0x00, 0x00, 0x00,             // mov eax, 0x33
		0x8e, 0xe8,                               // mov gs, ax
		0x65, 0xc7, 0x45, 0x04, 0xff, 0xff, 0xff, 0xff,
	});
	CpuState cpu{};
	cpu.execution_mode = ExecutionMode::compat32;
	cpu.compat_tls_bases[0] = 0xffff'f000;
	cpu.compat_tls_present = 1;
	cpu.gpr(Register::rbp) = 0x1080;
	const auto built = gadgets::build(
		memory, 0, ExecutionMode::compat32, 3);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"compat32 TLS segment block did not build"))
		return false;
	const auto result = gadgets::execute(built.program, cpu, memory, 3);
	return expect(result.event == uml::cpu::EngineEvent::continued &&
		result.retired_instructions == 3,
		"compat32 TLS segment block did not execute") &&
		expect(cpu.segment_selectors[5] == 0x33 &&
			cpu.gs_base == 0xffff'f000,
			"compat32 GS selector did not load its TLS base") &&
		expect(memory.dword(0x84) == 0xffff'ffff,
			"compat32 segmented linear address did not wrap at 32 bits");
}

[[nodiscard]] bool reject_program_mode_mismatch()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x40});
	const auto built = gadgets::build(
		memory, 0, ExecutionMode::compat32, 1);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"compat32 identity block did not build"))
		return false;
	CpuState cpu{};
	const auto mismatch = gadgets::execute(built.program, cpu, memory, 1);
	if (!expect(mismatch.event == uml::cpu::EngineEvent::fatal &&
		mismatch.retired_instructions == 0,
		"long64 CPU accepted a compat32 native stream"))
		return false;
	cpu.execution_mode = ExecutionMode::compat32;
	const auto match = gadgets::execute(built.program, cpu, memory, 1);
	return expect(match.event == uml::cpu::EngineEvent::continued &&
		match.retired_instructions == 1 &&
		cpu.gpr(Register::rax) == 1 && cpu.rip == 1,
		"compat32 CPU did not execute its matching native stream");
}

[[nodiscard]] bool chunk_repeating_string_for_interrupt()
{
	using namespace uml::cpu::x86_64;
	/* A repeated string operation has to hand the executor a boundary so a
	 * mapping change can take the execution read gate. The element loop
	 * counts elements. The run loop counts the bytes it moves instead: one
	 * run stands for thousands of elements, so counting runs would let a long
	 * copy hold the gate far longer than the element loop ever did.
	 */
	TestMemory descending;
	descending.load(0, {0xf3, 0xaa}); // rep stosb
	CpuState down{};
	down.gpr(Register::rax) = 0x5a;
	down.gpr(Register::rcx) = 4097;
	down.gpr(Register::rdi) = 8192;
	down.rflags |= rflags_direction;
	const auto element_built = gadgets::build(descending, 0, 1);
	if (!expect(element_built.status == gadgets::BuildStatus::built,
		"descending REP STOSB did not build"))
		return false;
	const auto element = gadgets::execute(
		element_built.program, down, descending, 1);
	if (!expect(element.event == uml::cpu::EngineEvent::continued &&
		element.retired_instructions == 0 && down.rip == 0 &&
		down.gpr(Register::rcx) == 1 &&
		down.gpr(Register::rdi) == 4096 &&
		descending.byte(8192) == 0x5a && descending.byte(4097) == 0x5a,
		"descending REP STOSB did not yield after its element budget"))
		return false;

	TestMemory memory{0, 270336};
	memory.load(0, {0xf3, 0xaa}); // rep stosb
	CpuState cpu{};
	cpu.gpr(Register::rax) = 0x5a;
	cpu.gpr(Register::rcx) = 262145;
	cpu.gpr(Register::rdi) = 4096;
	const auto built = gadgets::build(memory, 0, 1);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"REP STOSB did not build"))
		return false;
	const auto first = gadgets::execute(built.program, cpu, memory, 1);
	if (!expect(first.event == uml::cpu::EngineEvent::continued &&
		first.retired_instructions == 0 && cpu.rip == 0,
		"long REP STOSB did not yield before retiring") ||
		!expect(cpu.gpr(Register::rcx) == 1 &&
			cpu.gpr(Register::rdi) == 266240 &&
			memory.byte(4096) == 0x5a && memory.byte(266239) == 0x5a,
			"REP STOSB chunk did not preserve its partial state"))
		return false;
	/* 32-bit address arithmetic reaches the same run loop. Its yield
	 * boundary is the cheapest proof: the element loop would have stopped
	 * after 4096 elements rather than 256 KiB of them.
	 */
	TestMemory narrow{0, 270336};
	narrow.load(0, {0xf3, 0x67, 0xaa}); // addr32 rep stosb
	CpuState small{};
	small.gpr(Register::rax) = 0x5a;
	small.gpr(Register::rcx) = 262145;
	small.gpr(Register::rdi) = 4096;
	const auto narrow_built = gadgets::build(narrow, 0, 1);
	if (!expect(narrow_built.status == gadgets::BuildStatus::built,
		"32-bit REP STOSB did not build"))
		return false;
	const auto narrow_first = gadgets::execute(
		narrow_built.program, small, narrow, 1);
	if (!expect(narrow_first.event == uml::cpu::EngineEvent::continued &&
		small.gpr(Register::rcx) == 1 &&
		small.gpr(Register::rdi) == 266240 &&
		narrow.byte(266239) == 0x5a,
		"32-bit REP STOSB did not move its bytes in runs"))
		return false;

	unsigned int sequence = 1;
	const uml::cpu::InterruptToken interrupt{
		.sequence = &sequence,
		.snapshot = 0,
	};
	const auto interrupted = gadgets::execute(
		built.program, cpu, memory, 1, interrupt);
	if (!expect(interrupted.event == uml::cpu::EngineEvent::interrupt &&
		interrupted.retired_instructions == 0 &&
		cpu.gpr(Register::rcx) == 1 && cpu.rip == 0,
		"REP STOSB chunk did not expose its interrupt boundary"))
		return false;
	const auto finished = gadgets::execute(built.program, cpu, memory, 1);
	return expect(finished.event == uml::cpu::EngineEvent::continued &&
		finished.retired_instructions == 1 && cpu.rip == 2,
		"REP STOSB did not retire after resuming") &&
		expect(cpu.gpr(Register::rcx) == 0 &&
			cpu.gpr(Register::rdi) == 266241 &&
			memory.byte(266240) == 0x5a,
			"REP STOSB did not complete from its partial state");
}

[[nodiscard]] bool repeated_store_into_executable_page_publishes_its_run()
{
	using namespace uml::cpu::x86_64;
	/* A repeated store whose destination page can hold code is the one shape
	 * where a run has to do more than move bytes: it publishes the change and
	 * leaves through the instruction's own exit instead of chaining onward.
	 * Publishing once for a run rather than once per element is the only
	 * reason such a destination is worth running at all, so the result has to
	 * be the element loop's result -- every byte written, one code change
	 * published, and the block ended.
	 */
	TestMemory memory;
	memory.load(0, {0xf3, 0xaa}); // rep stosb
	memory.set_executable_page(0);
	CpuState cpu{};
	cpu.gpr(Register::rax) = 0x5a;
	cpu.gpr(Register::rcx) = 256;
	cpu.gpr(Register::rdi) = 0x800;
	gadgets::Executor executor;
	executor.prepare(memory, 1, 1, 1);
	/* Cached, so the destination page is on the shared code-page list. */
	const auto built = executor.select_program(
		memory, 0, ExecutionMode::long64, 1);
	if (!expect(built.program != nullptr,
		"executable-destination REP STOSB did not build"))
		return false;
	const auto result = gadgets::execute(
		*built.program, cpu, memory, executor, 64);
	if (!expect(result.event == uml::cpu::EngineEvent::continued &&
		result.retired_instructions == 1 && cpu.rip == 2 &&
		cpu.gpr(Register::rcx) == 0 && cpu.gpr(Register::rdi) == 0x900,
		"executable-destination REP STOSB did not end its block"))
		return false;
	bool written = true;
	for (std::uint64_t address = 0x800; address < 0x900; ++address)
		written = written && memory.byte(address) == 0x5a;
	return expect(written,
		"executable-destination REP STOSB did not write its run") &&
		expect(memory.byte(0x7ff) == 0 && memory.byte(0x900) == 0,
			"executable-destination REP STOSB wrote past its run") &&
		expect(memory.invalidations() == 1,
			"executable-destination REP STOSB did not publish once");
}

[[nodiscard]] bool repeated_move_faults_on_a_run_boundary()
{
	using namespace uml::cpu::x86_64;
	/* A run writes its architectural state back before starting the next
	 * one, so a translation that faults leaves the instruction resumable at
	 * exactly the byte it reached. Nothing else in the engine can check that:
	 * the element loop kept the same invariant one element at a time, and a
	 * run advances thousands of elements between writebacks.
	 */
	TestMemory memory;
	memory.load(0, {0xf3, 0xa4}); // rep movsb
	for (std::uint64_t address = 4096; address < 8192; ++address)
		memory.load(address, {static_cast<std::uint8_t>(address & 0xff)});
	CpuState cpu{};
	cpu.gpr(Register::rcx) = 8192;
	cpu.gpr(Register::rsi) = 4096;
	cpu.gpr(Register::rdi) = 12288;
	const auto built = gadgets::build(memory, 0, 1);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"faulting REP MOVSB did not build"))
		return false;
	const auto result = gadgets::execute(built.program, cpu, memory, 1);
	/* The first run fills the last mapped page; the second cannot translate
	 * its destination at all. */
	if (!expect(result.event == uml::cpu::EngineEvent::memory_fault &&
		cpu.rip == 0 && cpu.gpr(Register::rcx) == 4096 &&
		cpu.gpr(Register::rsi) == 8192 &&
		cpu.gpr(Register::rdi) == 16384,
		"faulting REP MOVSB did not stop on its run boundary"))
		return false;
	bool copied = true;
	for (std::uint64_t offset = 0; offset < 4096; ++offset)
		copied = copied && memory.byte(12288 + offset) ==
			memory.byte(4096 + offset);
	return expect(copied,
		"faulting REP MOVSB did not keep the bytes it had moved");
}

[[nodiscard]] bool bulk_string_runs_match_the_element_definition()
{
	using namespace uml::cpu::x86_64;
	/* The run loop is only a faster spelling of the element loop, so the two
	 * have to agree byte for byte and register for register across every
	 * shape that decides between them: a page boundary falling inside a run
	 * or inside one element, a destination overlapping its source in either
	 * direction, a descending direction, and counts on both sides of the
	 * length below which a run is not worth starting.
	 *
	 * Both are compared against the architectural definition rather than
	 * against each other, so a shape neither one handles is still caught.
	 */
	struct Case final {
		std::uint8_t prefix;
		std::uint8_t opcode;
		unsigned width;
		bool move;
		bool address32;
		const char *name;
	};
	static constexpr Case cases[] = {
		{0x00, 0xa4, 1, true, false, "rep movsb"},
		{0x66, 0xa5, 2, true, false, "rep movsw"},
		{0x00, 0xa5, 4, true, false, "rep movsd"},
		{0x48, 0xa5, 8, true, false, "rep movsq"},
		{0x00, 0xaa, 1, false, false, "rep stosb"},
		{0x66, 0xab, 2, false, false, "rep stosw"},
		{0x00, 0xab, 4, false, false, "rep stosd"},
		{0x48, 0xab, 8, false, false, "rep stosq"},
		/* 32-bit address arithmetic reaches the same bytes here but keeps
		 * its pointers narrow, which is a separate advance. */
		{0x00, 0xa4, 1, true, true, "a32 rep movsb"},
		{0x48, 0xa5, 8, true, true, "a32 rep movsq"},
		{0x00, 0xaa, 1, false, true, "a32 rep stosb"},
		{0x48, 0xab, 8, false, true, "a32 rep stosq"},
	};
	/* Distances from a page boundary that put a run's first byte, its last
	 * byte, or one whole element across it. */
	static constexpr std::int64_t offsets[] = {0, 1, 7, -1, -7, -9, -64};
	static constexpr std::int64_t leads[] = {
		-4096, -70, -33, -16, -8, -1, 0, 1, 8, 16, 33, 70, 4096};
	static constexpr std::uint64_t counts[] = {
		1, 4, 15, 16, 31, 32, 33, 63, 64, 65, 130, 520};

	constexpr std::uint64_t span = 65536;
	constexpr std::uint64_t low = 8192;
	constexpr std::uint64_t high = 57344;
	constexpr std::uint64_t anchor = 32768;
	constexpr std::uint64_t value = 0x0123'4567'89ab'cdefULL;
	std::vector<std::byte> pattern(span);
	for (std::uint64_t address = 0; address < span; ++address)
		pattern[address] = static_cast<std::byte>(
			(address * 7U) ^ (address >> 5U));
	std::vector<std::byte> model(span);
	char message[192];

	for (const auto &item : cases) {
		TestMemory memory{0, span};
		std::uint64_t length = 2;
		if (item.address32 && item.prefix != 0) {
			memory.load(0, {0xf3, 0x67, item.prefix, item.opcode});
			length = 4;
		} else if (item.address32) {
			memory.load(0, {0xf3, 0x67, item.opcode});
			length = 3;
		} else if (item.prefix != 0) {
			memory.load(0, {0xf3, item.prefix, item.opcode});
			length = 3;
		} else {
			memory.load(0, {0xf3, item.opcode});
		}
		const auto built = gadgets::build(memory, 0, 1);
		if (!expect(built.status == gadgets::BuildStatus::built, item.name))
			return false;

		for (const auto descending : {false, true})
		for (const auto offset : offsets)
		for (const auto lead : leads)
		for (const auto count : counts) {
			const auto reach = static_cast<std::int64_t>(
				count * item.width);
			const auto source =
				static_cast<std::int64_t>(anchor) + offset;
			const auto destination = source + lead;
			/* Ends of the two ranges, whichever way they run. */
			const auto source_first = descending ?
				source - reach + static_cast<std::int64_t>(item.width) :
				source;
			const auto destination_first = descending ?
				destination - reach +
					static_cast<std::int64_t>(item.width) :
				destination;
			const auto first = source_first < destination_first ?
				source_first : destination_first;
			const auto last =
				(source_first > destination_first ?
					source_first : destination_first) + reach;
			if (first < static_cast<std::int64_t>(low) ||
				last > static_cast<std::int64_t>(high))
				continue;

			/* Restore only the window the case can touch, plus a guard
			 * band a run that overshoots would land in. */
			const auto begin = static_cast<std::uint64_t>(first) - 64;
			const auto end = static_cast<std::uint64_t>(last) + 64;
			memory.fill(begin, std::span{pattern}.subspan(
				begin, end - begin));
			std::memcpy(model.data() + begin, pattern.data() + begin,
				end - begin);

			CpuState cpu{};
			cpu.gpr(Register::rax) = value;
			cpu.gpr(Register::rcx) = count;
			cpu.gpr(Register::rsi) = static_cast<std::uint64_t>(source);
			cpu.gpr(Register::rdi) =
				static_cast<std::uint64_t>(destination);
			if (descending)
				cpu.rflags |= rflags_direction;

			/* The architectural definition: one element at a time in
			 * address order, each read whole before it is written, so a
			 * later element sees what earlier ones wrote but no element
			 * overwrites its own source.
			 */
			auto si = source;
			auto di = destination;
			const auto step = descending ?
				-static_cast<std::int64_t>(item.width) :
				static_cast<std::int64_t>(item.width);
			for (auto left = count; left != 0; --left) {
				std::array<std::byte, 8> element{};
				for (unsigned index = 0; index < item.width; ++index)
					element[index] = item.move ?
						model[static_cast<std::size_t>(si) + index] :
						static_cast<std::byte>(
							(value >> (8U * index)) & 0xffU);
				for (unsigned index = 0; index < item.width; ++index)
					model[static_cast<std::size_t>(di) + index] =
						element[index];
				si += step;
				di += step;
			}

			unsigned entries = 0;
			bool ran = true;
			while (ran && cpu.rip != length) {
				const auto result = gadgets::execute(
					built.program, cpu, memory, 1);
				ran = result.event ==
					uml::cpu::EngineEvent::continued &&
					++entries <= 8192;
			}

			bool same = ran && cpu.gpr(Register::rcx) == 0 &&
				cpu.gpr(Register::rdi) ==
					static_cast<std::uint64_t>(di) &&
				(!item.move || cpu.gpr(Register::rsi) ==
					static_cast<std::uint64_t>(si));
			for (auto address = begin; same && address < end; ++address)
				same = memory.byte(address) ==
					static_cast<std::uint8_t>(model[address]);
			if (!same) {
				std::snprintf(message, sizeof(message),
					"%s%s diverged from the element definition "
					"(offset %lld lead %lld count %llu)",
					item.name, descending ? " down" : "",
					static_cast<long long>(offset),
					static_cast<long long>(lead),
					static_cast<unsigned long long>(count));
				return expect(false, message);
			}
		}
	}
	return true;
}
/* A 32- or 64-bit CMP, SUB, TEST or AND leaves host flags that describe the
 * x86 flags it defined, and the builder lets the very next condition read them
 * directly. The slot after that reads the same flags through the deferred
 * record instead, so the two bytes a pair of identical SETcc write must agree
 * for every producer, width, operand pair and condition. That equality is the
 * whole correctness claim of the fused path, and it needs no reference model:
 * the established path is the reference.
 */
[[nodiscard]] bool fused_conditions_match_the_deferred_evaluation()
{
	using namespace uml::cpu::x86_64;
	/* CMP, SUB, TEST, AND and ADD register forms, then INC, DEC and NEG. */
	static constexpr std::array<unsigned, 5> producers{
		0x39, 0x29, 0x85, 0x21, 0x01};
	static constexpr std::array<std::array<unsigned, 2>, 3> unary{{
		{0xff, 0xc0}, {0xff, 0xc8}, {0xf7, 0xd8}}};
	static constexpr std::array<std::uint64_t, 12> values{
		0U, 1U, 2U, 0xffU, 0x7fff'ffffULL, 0x8000'0000ULL,
		0xffff'ffffULL, 0x7fff'ffff'ffff'ffffULL, 0x8000'0000'0000'0000ULL,
		~std::uint64_t{}, 0x5555'5555'5555'5555ULL, 0xaaaa'aaaa'aaaa'aaaaULL,
	};
	for (std::size_t index = 0; index < producers.size() + unary.size();
		++index) {
		const bool binary = index < producers.size();
		for (const unsigned width : {8U, 16U, 32U, 64U}) {
			for (unsigned condition = 0; condition < 16U; ++condition) {
				for (const auto left : values) {
					for (const auto right : values) {
						if (!binary && right != values.front())
							break;
						std::vector<std::byte> code;
						const auto push = [&code](unsigned byte) {
							code.push_back(static_cast<std::byte>(byte));
						};
						if (width == 16U)
							push(0x66);
						if (width == 64U)
							push(0x48);
						if (binary) {
							const auto producer = producers[index];
							push(width == 8U ? producer - 1U : producer);
							push(0xd8);
						} else {
							const auto &form = unary[index - producers.size()];
							push(width == 8U ? form[0] - 1U : form[0]);
							push(form[1]);
						}
						/* SYSCALL writes its return address to RCX and
						 * the flags to R11, so the two results live in RDX
						 * and RBX. */
						push(0x0f);
						push(0x90U + condition);
						push(0xc2);
						push(0x0f);
						push(0x90U + condition);
						push(0xc3);
						push(0x0f);
						push(0x05);
						TestMemory memory;
						memory.fill(0, code);
						const auto built = gadgets::build(memory, 0);
						if (!expect(built.status ==
							gadgets::BuildStatus::built,
							"fused condition program did not build"))
							return false;
						CpuState cpu{};
						cpu.gpr(Register::rax) = left;
						cpu.gpr(Register::rbx) = right;
						gadgets::Executor executor;
						executor.prepare(memory, 1, 1, 1);
						const auto result = gadgets::execute(
							built.program, cpu, memory, executor, 64);
						if (!expect(result.event ==
							uml::cpu::EngineEvent::system_call,
							"fused condition program did not reach SYSCALL"))
							return false;
						const auto fused =
							cpu.gpr(Register::rdx) & 0xffU;
						const auto deferred =
							cpu.gpr(Register::rbx) & 0xffU;
						if (fused != deferred) {
							char message[160];
							std::snprintf(message, sizeof(message),
								"a fused condition disagreed with the "
								"deferred evaluation of the same flags "
								"(producer %zu width %u condition %u left "
								"%#llx right %#llx: fused %u deferred %u)",
								index, width, condition,
								static_cast<unsigned long long>(left),
								static_cast<unsigned long long>(right),
								static_cast<unsigned>(fused),
								static_cast<unsigned>(deferred));
							return expect(false, message);
						}
					}
				}
			}
		}
	}
	return true;
}

/* INC and DEC preserve the carry flag. When the builder cannot see the
 * producer that defined it -- a store or a block boundary sits between -- the
 * gadget no longer settles it: it parks the producer's record in the carry
 * slot and leaves the carry to whoever reads it through the INC or DEC record.
 * Every reader must see the producer's carry: the carry materializer (ADC),
 * the full one (LAHF), the C++ one (an exit), a second INC or DEC that
 * inherits the pending bit, an override riding on the record (BT), and the
 * memory form. The other five flags come from the INC or DEC itself.
 */
[[nodiscard]] bool lazy_carry_reaches_every_reader()
{
	using namespace uml::cpu::x86_64;
	static constexpr std::array<std::uint64_t, 6> values{
		0U, 1U, 0x7fU, 0x80U, 0xffU, ~std::uint64_t{}};
	enum class Reader { adc, lahf, exit, twice_adc, bt_adc, memory_adc };
	static constexpr std::array<Reader, 6> readers{Reader::adc, Reader::lahf,
		Reader::exit, Reader::twice_adc, Reader::bt_adc, Reader::memory_adc};
	for (const unsigned width : {8U, 16U, 32U, 64U}) {
		const auto mask = width == 64U ? ~std::uint64_t{} :
			(std::uint64_t{1} << width) - 1U;
		for (const bool subtract : {false, true})
		for (const bool decrement : {false, true})
		for (const bool jump : {false, true})
		for (const auto reader : readers)
		for (const auto left : values)
		for (const auto right : values) {
			std::vector<std::byte> code;
			const auto push = [&code](std::initializer_list<unsigned> bytes) {
				for (const auto byte : bytes)
					code.push_back(static_cast<std::byte>(byte));
			};
			const auto prefix = [&]() {
				if (width == 16U)
					push({0x66});
				if (width == 64U)
					push({0x48});
			};
			/* ADD or SUB rax, rbx at the width defines the carry. */
			prefix();
			push({width == 8U ? (subtract ? 0x28U : 0x00U) :
				(subtract ? 0x29U : 0x01U), 0xd8});
			/* A store, or a block boundary, hides it from the builder. */
			if (jump)
				push({0xeb, 0x00});
			else
				push({0x48, 0x89, 0x0f});
			/* INC or DEC of rcx, or of [rdi]. */
			const unsigned unary = width == 8U ? 0xfeU : 0xffU;
			const unsigned modrm = reader == Reader::memory_adc ?
				(decrement ? 0x0fU : 0x07U) : (decrement ? 0xc9U : 0xc1U);
			prefix();
			push({unary, modrm});
			switch (reader) {
			case Reader::twice_adc:
				prefix();
				push({unary, modrm});
				[[fallthrough]];
			case Reader::adc:
			case Reader::memory_adc:
				push({0x48, 0x83, 0xd2, 0x00}); /* adc rdx, 0 */
				break;
			case Reader::bt_adc:
				/* bt rcx, 1: a carry override on the INC or DEC record. */
				push({0x48, 0x0f, 0xba, 0xe1, 0x01, 0x48, 0x83, 0xd2, 0x00});
				break;
			case Reader::lahf:
				push({0x9f});
				break;
			case Reader::exit:
				break;
			}
			push({0x0f, 0x05});
			TestMemory memory;
			memory.fill(0, code);
			CpuState cpu{};
			cpu.gpr(Register::rax) = left;
			cpu.gpr(Register::rbx) = right;
			cpu.gpr(Register::rdi) = 0x2000;
			gadgets::Executor executor;
			executor.prepare(memory, 1, 1, 1);
			bool reached = false;
			for (unsigned step = 0; step < 8U; ++step) {
				const auto selected = executor.select_program(
					memory, cpu.rip, ExecutionMode::long64);
				if (!expect(selected.status == gadgets::BuildStatus::built &&
					selected.program != nullptr,
					"lazy carry program did not build"))
					return false;
				const auto result = gadgets::execute(
					*selected.program, cpu, memory, executor, 64);
				if (result.event == uml::cpu::EngineEvent::system_call) {
					reached = true;
					break;
				}
				if (!expect(result.event == uml::cpu::EngineEvent::continued,
					"lazy carry program left unexpectedly"))
					return false;
			}
			if (!expect(reached, "lazy carry program did not reach SYSCALL"))
				return false;
			const auto l = left & mask;
			const auto r = right & mask;
			const std::uint64_t carry = subtract ? (l < r ? 1U : 0U) :
				(((l + r) & mask) < l ? 1U : 0U);
			std::uint64_t expected = carry;
			std::uint64_t observed = 0;
			switch (reader) {
			case Reader::adc:
			case Reader::twice_adc:
			case Reader::memory_adc:
				observed = cpu.gpr(Register::rdx);
				break;
			case Reader::bt_adc:
				/* Bit 1 of rcx: clear after INC from zero, set after DEC. */
				expected = decrement ? 1U : 0U;
				observed = cpu.gpr(Register::rdx);
				break;
			case Reader::lahf:
				/* SF ZF 0 AF 0 PF 1 CF of the INC or DEC from zero. */
				expected = (decrement ? 0x96U : 0x02U) | carry;
				observed = (cpu.gpr(Register::rax) >> 8U) & 0xffU;
				break;
			case Reader::exit:
				materialize_flags(cpu);
				observed = cpu.rflags & 1U;
				break;
			}
			if (reader == Reader::memory_adc) {
				const auto stored = memory.byte(0x2000);
				if (!expect(stored == (decrement ? 0xffU : 0x01U),
					"the memory INC or DEC did not write its result"))
					return false;
			}
			if (observed != expected) {
				char message[200];
				std::snprintf(message, sizeof(message),
					"a lazily preserved carry reached a reader wrong "
					"(width %u %s %s %s reader %d left %#llx right %#llx: "
					"expected %#llx observed %#llx)",
					width, subtract ? "sub" : "add",
					jump ? "jump" : "store", decrement ? "dec" : "inc",
					static_cast<int>(reader),
					static_cast<unsigned long long>(left),
					static_cast<unsigned long long>(right),
					static_cast<unsigned long long>(expected),
					static_cast<unsigned long long>(observed));
				return expect(false, message);
			}
		}
	}
	return true;
}

/* The equality above holds whether or not the builder actually fuses, so pin
 * which pairs it fuses. A NOP between the producer and the consumer resets the
 * host flags the builder tracks, so the same consumer keeps the deferred
 * gadget; the entry it names differs exactly where fusion is legal. Parity has
 * no host-flag form, and after a logical operation neither do the four
 * conditions that read the carry.
 */
[[nodiscard]] bool fused_conditions_cover_exactly_the_legal_pairs()
{
	using namespace uml::cpu::x86_64;
	static constexpr std::array<unsigned, 5> opcodes{
		0x39, 0x29, 0x85, 0x21, 0x01};
	/* The producer's stream length depends on which bank the builder chose,
	 * so find the consumer where the two streams part: the plain stream holds
	 * the NOP entry there, the fused one the consumer's entry. */
	const auto consumer_index = [](std::span<const std::uintptr_t> fused,
		std::span<const std::uintptr_t> plain) {
		std::size_t index = 0;
		while (index < fused.size() && index < plain.size() &&
			fused[index] == plain[index])
			++index;
		return index;
	};
	for (std::size_t index = 0; index < opcodes.size(); ++index) {
		const bool logical = index == 2 || index == 3;
		for (const unsigned width : {8U, 16U, 32U, 64U}) {
			for (unsigned condition = 0; condition < 16U; ++condition) {
				const bool carry_condition = condition >= 2U && condition <= 3U;
				const bool ordered_carry =
					condition >= 6U && condition <= 7U;
				const bool parity = condition >= 10U && condition <= 11U;
				/* The register-baked bank leaves host flags at every width;
				 * its narrow entries compare the operands shifted to the top
				 * of a 32-bit register. */
				const bool add = index == 4;
				const bool expected_fusion = !parity &&
					!(logical && (carry_condition || ordered_carry)) &&
					!(add && ordered_carry);
				std::vector<std::byte> fused;
				std::vector<std::byte> plain;
				const auto emit = [&](std::vector<std::byte> &code,
					bool barrier) {
					const auto push = [&code](unsigned byte) {
						code.push_back(static_cast<std::byte>(byte));
					};
					if (width == 16U)
						push(0x66);
					if (width == 64U)
						push(0x48);
					push(width == 8U ? opcodes[index] - 1U : opcodes[index]);
					push(0xd8);
					if (barrier)
						push(0x90);
					push(0x0f);
					push(0x90U + condition);
					push(0xc1);
					push(0x0f);
					push(0x05);
				};
				emit(fused, false);
				emit(plain, true);
				TestMemory fused_memory;
				TestMemory plain_memory;
				fused_memory.fill(0, fused);
				plain_memory.fill(0, plain);
				const auto built = gadgets::build(fused_memory, 0);
				const auto separated = gadgets::build(plain_memory, 0);
				if (!expect(built.status == gadgets::BuildStatus::built &&
					separated.status == gadgets::BuildStatus::built,
					"condition pairing program did not build"))
					return false;
				const auto fused_stream = built.program.native_stream();
				const auto plain_stream = separated.program.native_stream();
				if (!expect(fused_stream.size() + 1U == plain_stream.size() &&
					fused_stream.size() > 3U,
					"condition pairing changed the stream layout"))
					return false;
				const auto consumer = consumer_index(fused_stream, plain_stream);
				if (!expect(consumer + 1U < fused_stream.size(),
					"condition pairing streams do not part at the consumer"))
					return false;
				const bool differs =
					fused_stream[consumer] != plain_stream[consumer + 1U];
				if (!expect(differs == expected_fusion,
					"the builder fused a condition it should not have, or "
					"declined one it should have fused"))
					return false;
				/* The same pairing, with a conditional jump as the consumer
				 * rather than a SETcc. */
				std::vector<std::byte> fused_jump;
				std::vector<std::byte> plain_jump;
				const auto emit_jump = [&](std::vector<std::byte> &code,
					bool barrier) {
					const auto push = [&code](unsigned byte) {
						code.push_back(static_cast<std::byte>(byte));
					};
					if (width == 16U)
						push(0x66);
					if (width == 64U)
						push(0x48);
					push(width == 8U ? opcodes[index] - 1U : opcodes[index]);
					push(0xd8);
					if (barrier)
						push(0x90);
					push(0x70U + condition);
					push(0x05);
					push(0xba);
					push(0x01);
					push(0x00);
					push(0x00);
					push(0x00);
					push(0x0f);
					push(0x05);
				};
				emit_jump(fused_jump, false);
				emit_jump(plain_jump, true);
				TestMemory fused_jump_memory;
				TestMemory plain_jump_memory;
				fused_jump_memory.fill(0, fused_jump);
				plain_jump_memory.fill(0, plain_jump);
				const auto jump_built = gadgets::build(fused_jump_memory, 0);
				const auto jump_separated =
					gadgets::build(plain_jump_memory, 0);
				if (!expect(jump_built.status == gadgets::BuildStatus::built &&
					jump_separated.status == gadgets::BuildStatus::built,
					"jump pairing program did not build"))
					return false;
				const auto jump_fused = jump_built.program.native_stream();
				const auto jump_plain =
					jump_separated.program.native_stream();
				if (!expect(jump_fused.size() + 1U == jump_plain.size() &&
					jump_fused.size() > 4U,
					"jump pairing changed the stream layout"))
					return false;
				const auto jump_consumer = consumer_index(jump_fused, jump_plain);
				if (!expect(jump_consumer + 1U < jump_fused.size(),
					"jump pairing streams do not part at the consumer"))
					return false;
				if (!expect((jump_fused[jump_consumer] !=
					jump_plain[jump_consumer + 1U]) == expected_fusion,
					"the builder fused a conditional jump it should not have, "
					"or declined one it should have fused"))
					return false;
			}
		}
	}
	return true;
}

/* The SETcc equality pins the condition mapping, which the conditional jump
 * shares. What it does not pin is the jump's own plumbing: a fused jump
 * selects between two stream words with the host condition instead of a
 * materialized bit, and selecting the wrong one would send control to the
 * wrong successor rather than produce a wrong byte. Run the pair for real,
 * across block boundaries, with the two edges leading to different results.
 */
[[nodiscard]] bool fused_conditional_jumps_take_the_same_edge()
{
	using namespace uml::cpu::x86_64;
	static constexpr std::array<unsigned, 4> producers{0x39, 0x29, 0x85, 0x21};
	static constexpr std::array<std::uint64_t, 6> values{
		0U, 1U, 0x7fff'ffff'ffff'ffffULL, 0x8000'0000'0000'0000ULL,
		~std::uint64_t{}, 0x5555'5555'5555'5555ULL};
	for (const auto producer : producers) {
		for (const unsigned width : {32U, 64U}) {
			for (unsigned condition = 0; condition < 16U; ++condition) {
				for (const auto left : values) {
					for (const auto right : values) {
						std::array<std::uint64_t, 2> taken{};
						for (unsigned barrier = 0; barrier < 2U; ++barrier) {
							std::vector<std::byte> code;
							const auto push = [&code](unsigned byte) {
								code.push_back(
									static_cast<std::byte>(byte));
							};
							if (width == 64U)
								push(0x48);
							push(producer);
							push(0xd8);
							if (barrier != 0U)
								push(0x90);
							/* j<cc> over a five-byte MOV. */
							push(0x70U + condition);
							push(0x05);
							push(0xba);
							push(0x01);
							push(0x00);
							push(0x00);
							push(0x00);
							push(0x0f);
							push(0x05);
							TestMemory memory;
							memory.fill(0, code);
							CpuState cpu{};
							cpu.gpr(Register::rax) = left;
							cpu.gpr(Register::rbx) = right;
							gadgets::Executor executor;
							executor.prepare(memory, 1, 1, 1);
							bool reached = false;
							for (unsigned step = 0; step < 8U; ++step) {
								const auto selected = executor.select_program(
									memory, cpu.rip, ExecutionMode::long64);
								if (!expect(selected.status ==
									gadgets::BuildStatus::built &&
									selected.program != nullptr,
									"fused jump program did not build"))
									return false;
								const auto result = gadgets::execute(
									*selected.program, cpu, memory, executor,
									64);
								if (result.event ==
									uml::cpu::EngineEvent::system_call) {
									reached = true;
									break;
								}
								if (!expect(result.event ==
									uml::cpu::EngineEvent::continued,
									"fused jump program left unexpectedly"))
									return false;
							}
							if (!expect(reached,
								"fused jump program did not reach SYSCALL"))
								return false;
							taken[barrier] = cpu.gpr(Register::rdx) & 0xffU;
						}
						if (!expect(taken[0] == taken[1],
							"a fused conditional jump took a different edge "
							"than the deferred evaluation of the same flags"))
							return false;
					}
				}
			}
		}
	}
	return true;
}

/* The host reports the emulated CPU to the guest kernel from cpuid.hpp, and
 * the guest reads the same CPU by executing CPUID. Both answers must be the
 * same answer: whatever /proc/cpuinfo says a program may use, CPUID has to
 * agree, or a feature resolver and a capability script reach opposite
 * conclusions about the same machine. Nothing keeps the table and the gadget
 * together except this test, so it walks every leaf either one claims.
 */
[[nodiscard]] bool cpuid_table_matches_the_gadget()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x0f, 0xa2}); // cpuid
	const auto built = gadgets::build(memory, 0, ExecutionMode::long64, 1);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"CPUID did not build"))
		return false;

	/* Every leaf the table answers, the leaf above each maximum, and a
	 * subleaf of each enumerating leaf that must read as zero. */
	static constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 15>
		probed{{
			{0U, 0U}, {1U, 0U}, {7U, 0U}, {7U, 1U},
			{13U, 0U}, {13U, 1U}, {13U, 2U}, {13U, 3U},
			{2U, 0U}, {14U, 0U},
			{0x8000'0000U, 0U}, {0x8000'0001U, 0U},
			{0x8000'0002U, 0U}, {0x8000'0003U, 0U},
			{0x8000'0004U, 0U},
		}};
	for (const auto &[leaf, subleaf] : probed) {
		CpuState cpu{};
		cpu.gpr(Register::rax) = leaf;
		cpu.gpr(Register::rcx) = subleaf;
		const auto result =
			gadgets::execute(built.program, cpu, memory, 1);
		const auto table = cpuid(leaf, subleaf);
		char message[128];
		std::snprintf(message, sizeof message,
			"CPUID leaf 0x%08x subleaf %u disagrees with cpuid.hpp",
			leaf, subleaf);
		if (!expect(result.event == uml::cpu::EngineEvent::continued &&
				static_cast<std::uint32_t>(
					cpu.gpr(Register::rax)) == table.eax &&
				static_cast<std::uint32_t>(
					cpu.gpr(Register::rbx)) == table.ebx &&
				static_cast<std::uint32_t>(
					cpu.gpr(Register::rcx)) == table.ecx &&
				static_cast<std::uint32_t>(
					cpu.gpr(Register::rdx)) == table.edx,
			message))
			return false;
	}

	/* The brand string is what a guest prints as the model name, so it is
	 * read the way a guest reads it: little-endian bytes, EAX through EDX,
	 * across the three brand leaves. */
	char brand[49] = {};
	std::size_t at = 0;
	for (std::uint32_t leaf = 0x8000'0002U; leaf <= 0x8000'0004U; ++leaf) {
		const auto part = cpuid(leaf, 0U);
		for (const auto word : {part.eax, part.ebx, part.ecx, part.edx})
			for (unsigned byte = 0; byte < 4U; ++byte)
				brand[at++] = static_cast<char>(
					(word >> (8U * byte)) & 0xffU);
	}
	return expect(std::string_view(brand) == uml::cpu::guest_cpu_model_name,
		"the CPUID brand string is not this virtual CPU's own name");
}

[[nodiscard]] bool preserve_precise_memory_fault()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0x48, 0x8b, 0x03});
	CpuState cpu{};
	cpu.gpr(Register::rbx) = 16383;
	const auto built = gadgets::build(memory, 0, 1);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"faulting load did not build"))
		return false;
	const auto result = gadgets::execute(built.program, cpu, memory, 1);
	return expect(result.event == uml::cpu::EngineEvent::memory_fault,
		"faulting load did not exit as a memory fault") &&
		expect(result.fault.address == 16383 &&
			result.fault.access == uml::memory::MemoryAccess::read,
			"faulting load lost precise metadata") &&
		expect(result.retired_instructions == 0 && cpu.rip == 0,
			"faulting load retired or advanced RIP");
}

[[nodiscard]] bool retire_valid_prefix_before_decode_fault()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x2a, 0x00, 0x00, 0x00,
		0x0f, 0x0b,
	});
	CpuState cpu{};
	const auto prefix = gadgets::build(memory, 0);
	if (!expect(prefix.status == gadgets::BuildStatus::built,
		"valid prefix before UD2 did not build"))
		return false;
	const auto prefix_result = gadgets::execute(
		prefix.program, cpu, memory, 64);
	if (!expect(prefix_result.event == uml::cpu::EngineEvent::continued &&
		prefix_result.retired_instructions == 1 && cpu.rip == 5 &&
		cpu.gpr(Register::rax) == 42,
		"valid prefix before UD2 did not retire first"))
		return false;
	const auto illegal = gadgets::build(memory, cpu.rip);
	return expect(illegal.status == gadgets::BuildStatus::illegal_instruction,
		"UD2 boundary was not reported after the valid prefix");
}

[[nodiscard]] bool migrated_gadgets_are_native_only()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0x90,
		0x48, 0x0f, 0xc8,
		0xf8,
		0x9f,
		0x48, 0xd1, 0xe0,
		0x48, 0x0f, 0xa4, 0xc8, 0x01,
		0x48, 0x0f, 0xaf, 0xc1,
		0x48, 0xf7, 0xe1,
		0x48, 0xf7, 0xf1,
		0x48, 0xf7, 0xf9,
		0x48, 0x0f, 0xa3, 0xc8,
		0x48, 0x0f, 0xab, 0xc8,
		0x48, 0x0f, 0xb3, 0xc8,
		0x48, 0x0f, 0xbb, 0xc8,
		0x48, 0x0f, 0xbc, 0xc1,
		0x48, 0x0f, 0xbd, 0xc1,
		0xf3, 0x48, 0x0f, 0xbc, 0xc1,
		0xf3, 0x48, 0x0f, 0xbd, 0xc1,
		0xf3, 0x48, 0x0f, 0xb8, 0xc1,
		0x48, 0x0f, 0xb1, 0xc8,
		0x48, 0x0f, 0xc7, 0x0b,
		0x48, 0x0f, 0xc1, 0xc8,
		0x48, 0x0f, 0x38, 0xf0, 0x03,
		0xf2, 0x48, 0x0f, 0x38, 0xf1, 0xc1,
		0x66, 0x48, 0x0f, 0x38, 0xf6, 0xc1,
		0xf3, 0x48, 0x0f, 0x38, 0xf6, 0xc1,
		0x0f, 0xae, 0x03,
		0x0f, 0xae, 0x23,
		0xeb, 0x00,
	});
	CpuState cpu{};
	cpu.gpr(Register::rax) = 0x0102'0304'0506'0708ULL;
	const auto built = gadgets::build(memory, 0);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"native-only block did not build") ||
		!expect(built.program.instruction_count() == 29,
			"native-only block has the wrong instruction count") ||
		!expect(!built.program.native_stream().empty(),
			"native-only block has no threaded assembly stream"))
		return false;
	const auto result = gadgets::execute(built.program, cpu, memory, 2);
	return expect(result.event == uml::cpu::EngineEvent::continued &&
		result.retired_instructions == 2 &&
		result.instruction_budget_exhausted,
		"limited native stream did not exhaust its budget") &&
		expect(cpu.rip == 4 &&
			cpu.gpr(Register::rax) == 0x0807'0605'0403'0201ULL,
			"limited native stream produced the wrong state");
}

[[nodiscard]] bool native_divide_traps_precisely()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(100, {0x48, 0xf7, 0xf1});
	CpuState cpu{};
	cpu.rip = 100;
	cpu.gpr(Register::rax) = 42;
	cpu.gpr(Register::rdx) = 0;
	cpu.gpr(Register::rcx) = 0;
	const auto built = gadgets::build(memory, 100, 1);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"native DIV trap block did not build"))
		return false;
	const auto result = gadgets::execute(built.program, cpu, memory, 1);
	return expect(result.event == uml::cpu::EngineEvent::floating_point_trap,
		"divide by zero did not return the x86 divide trap") &&
		expect(result.retired_instructions == 0 && cpu.rip == 100,
			"divide trap retired or lost its precise RIP") &&
		expect(cpu.gpr(Register::rax) == 42 &&
			cpu.gpr(Register::rdx) == 0,
			"divide trap modified the dividend registers");
}

[[nodiscard]] bool native_into_reports_overflow_precisely()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(100, {0xce});
	const auto built = gadgets::build(
		memory, 100, ExecutionMode::compat32, 1);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"native INTO block did not build"))
		return false;
	CpuState taken{};
	taken.execution_mode = ExecutionMode::compat32;
	taken.rip = 100;
	taken.rflags |= rflags_overflow;
	const auto taken_result = gadgets::execute(
		built.program, taken, memory, 1);
	if (!expect(taken_result.event == uml::cpu::EngineEvent::overflow,
		"taken INTO did not return the overflow trap") ||
		!expect(taken_result.retired_instructions == 1 && taken.rip == 101,
			"taken INTO did not preserve trap retirement semantics"))
		return false;
	CpuState clear{};
	clear.execution_mode = ExecutionMode::compat32;
	clear.rip = 100;
	const auto clear_result = gadgets::execute(
		built.program, clear, memory, 1);
	return expect(clear_result.event == uml::cpu::EngineEvent::continued &&
		clear_result.instruction_budget_exhausted &&
		clear_result.retired_instructions == 1 && clear.rip == 101,
		"clear-overflow INTO did not retire normally");
}

/* Clang's AVX2 path uses VMOVLHPS with the r/m source aliased to the
 * destination while saving its lexer stack.  The gadget must capture that
 * source lane before replacing the destination with the VEX merge operand.
 */
[[nodiscard]] bool vex_vmovlhps_preserves_aliased_source()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xc5, 0xe8, 0x16, 0xc9, // vmovlhps xmm1,xmm2,xmm1
		0x0f, 0x05,             // syscall
	});
	const auto built = gadgets::build(memory, 0);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"aliased VMOVLHPS sequence did not build"))
		return false;
	CpuState cpu{};
	constexpr std::uint64_t original_destination_low = 0x1122'3344'5566'7788;
	constexpr std::uint64_t merge_low = 0x0102'0304'0506'0708;
	cpu.vectors[1].fill(std::byte{0xa5});
	cpu.vectors[2].fill(std::byte{0x5a});
	std::memcpy(cpu.vectors[1].data(), &original_destination_low,
		sizeof(original_destination_low));
	std::memcpy(cpu.vectors[2].data(), &merge_low, sizeof(merge_low));
	const auto result = gadgets::execute(built.program, cpu, memory, 2);
	std::array<std::uint64_t, 4> actual{};
	std::memcpy(actual.data(), cpu.vectors[1].data(), sizeof(actual));
	return expect(result.event == uml::cpu::EngineEvent::system_call &&
			result.retired_instructions == 2 && cpu.rip == 6,
			"aliased VMOVLHPS did not reach SYSCALL") &&
		expect(actual[0] == merge_low &&
			actual[1] == original_destination_low,
			"aliased VMOVLHPS read its source after writing the destination") &&
		expect(actual[2] == 0 && actual[3] == 0,
			"VEX VMOVLHPS did not clear the upper YMM lane");
}

[[nodiscard]] bool vector_invalid_results_use_x86_indefinite_nan()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {
		0xf2, 0x0f, 0x5e, 0xc1, // divsd xmm0,xmm1: +0 / +0
		0xf3, 0x0f, 0x51, 0xd3, // sqrtss xmm2,xmm3: sqrt(-1)
		0xf3, 0x0f, 0x52, 0xe5, // rsqrtss xmm4,xmm5: 1 / sqrt(-4)
		0x0f, 0x05,             // syscall
	});
	const auto built = gadgets::build(memory, 0);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"invalid vector floating-point sequence did not build"))
		return false;
	CpuState cpu{};
	constexpr std::uint32_t negative_one = 0xbf80'0000;
	constexpr std::uint32_t negative_four = 0xc080'0000;
	std::memcpy(cpu.vectors[3].data(), &negative_one, sizeof(negative_one));
	std::memcpy(cpu.vectors[5].data(), &negative_four, sizeof(negative_four));
	const auto result = gadgets::execute(built.program, cpu, memory, 4);
	std::uint64_t quotient{};
	std::uint32_t square_root{};
	std::uint32_t reciprocal_square_root{};
	std::memcpy(&quotient, cpu.vectors[0].data(), sizeof(quotient));
	std::memcpy(&square_root, cpu.vectors[2].data(), sizeof(square_root));
	std::memcpy(&reciprocal_square_root, cpu.vectors[4].data(),
		sizeof(reciprocal_square_root));
	return expect(result.event == uml::cpu::EngineEvent::system_call &&
			result.retired_instructions == 4 && cpu.rip == 14,
			"invalid vector floating-point sequence did not reach SYSCALL") &&
		expect(quotient == 0xfff8'0000'0000'0000ULL,
			"DIVSD 0/0 did not return the x86 double indefinite NaN") &&
		expect(square_root == 0xffc0'0000,
			"SQRTSS of a negative value did not return the x86 indefinite NaN") &&
		expect(reciprocal_square_root == 0xffc0'0000,
			"RSQRTSS of a negative value did not return the x86 indefinite NaN");
}

/* The gadget a block selects for its first instruction. */
[[nodiscard]] std::uintptr_t first_native_entry(
	std::initializer_list<std::uint8_t> code)
{
	TestMemory memory;
	memory.load(0, code);
	const auto built = uml::cpu::x86_64::gadgets::build(memory, 0);
	if (built.status != uml::cpu::x86_64::gadgets::BuildStatus::built ||
		built.program.native_stream().empty())
		return 0;
	return built.program.native_stream()[0];
}

[[nodiscard]] std::uintptr_t gadget_entry(void (*entry)()) noexcept
{
	return reinterpret_cast<std::uintptr_t>(entry);
}

/* A LOCK-prefixed memory update has to reach the exclusive-loop gadget.  The
 * plain gadget loads and stores separately, which loses a sibling vCPU's
 * update, so pin the selection rather than only the arithmetic result.
 */
[[nodiscard]] bool locked_memory_updates_select_exclusive_gadgets()
{
	using namespace uml::cpu::x86_64::gadgets;
	/* Every block ends in SYSCALL so it terminates at a known boundary. */
	return expect(first_native_entry({0xf0, 0xff, 0x00, 0x0f, 0x05}) ==
		gadget_entry(native::uml_x86_64_gadget_increment_32_m_locked),
		"LOCK INC did not select the exclusive increment gadget") &&
		expect(first_native_entry({0xf0, 0x01, 0x08, 0x0f, 0x05}) ==
			gadget_entry(native::uml_x86_64_gadget_add_32_mr_locked),
			"LOCK ADD register did not select the exclusive gadget") &&
		expect(first_native_entry({0xf0, 0x83, 0x00, 0x01, 0x0f, 0x05}) ==
			gadget_entry(native::uml_x86_64_gadget_add_32_mi_locked),
			"LOCK ADD immediate did not select the exclusive gadget") &&
		expect(first_native_entry({0xf0, 0x0f, 0xab, 0x08, 0x0f, 0x05}) ==
			gadget_entry(native::uml_x86_64_gadget_bts_32_mr_locked),
			"LOCK BTS did not select the exclusive gadget") &&
		/* CMPXCHG, XADD and XCHG against memory are atomic on x86 whether
		 * or not the prefix is present, so they have no separate form.
		 */
		expect(first_native_entry({0xf0, 0x0f, 0xb1, 0x08, 0x0f, 0x05}) ==
			gadget_entry(native::uml_x86_64_gadget_cmpxchg_32_mr),
			"LOCK CMPXCHG did not select the memory gadget") &&
		expect(first_native_entry({0xf0, 0x0f, 0xc1, 0x08, 0x0f, 0x05}) ==
			gadget_entry(native::uml_x86_64_gadget_xadd_32_mr),
			"LOCK XADD did not select the memory gadget") &&
		expect(first_native_entry({0x87, 0x08, 0x0f, 0x05}) ==
			gadget_entry(native::uml_x86_64_gadget_exchange_32_mr),
			"XCHG did not select the memory gadget") &&
		/* An unprefixed update keeps a cheaper non-exclusive gadget: the
		 * ordinary one, or the register-baked bank's ECX-into-[RAX] entry. */
		expect(first_native_entry({0xff, 0x00, 0x0f, 0x05}) ==
			gadget_entry(native::uml_x86_64_gadget_increment_32_m),
			"unlocked INC lost its plain gadget") &&
		expect(first_native_entry({0x01, 0x08, 0x0f, 0x05}) ==
				gadget_entry(native::uml_x86_64_gadget_add_32_mr) ||
			first_native_entry({0x01, 0x08, 0x0f, 0x05}) ==
				baked::uml_x86_64_baked_add_32_mr[1U * baked::base_slots],
			"unlocked ADD lost its plain gadget");
}

/* glibc's pthread_rwlock_rdlock fast path adds one reader with LOCK XADD,
 * converts the returned old value to the prospective state, and checks its
 * sign. Keep the exact instruction sequence that pressure-vessel executes so
 * a width, source-register, or deferred-flag regression is observable without
 * a desktop boot.
 */
[[nodiscard]] bool glibc_rwlock_xadd_fast_path_is_exact()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t lock = 0x800;
	TestMemory memory;
	memory.load(0, {
		0xb8, 0x08, 0x00, 0x00, 0x00, // mov eax,8
		0xf0, 0x0f, 0xc1, 0x03,       // lock xadd [rbx],eax
		0x83, 0xc0, 0x08,             // add eax,8
		0x85, 0xc0,                   // test eax,eax
		0x0f, 0x05,                   // syscall
	});
	const auto built = gadgets::build(memory, 0);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"glibc rwlock XADD sequence did not build"))
		return false;
	CpuState cpu{};
	cpu.gpr(Register::rbx) = lock;
	const auto result = gadgets::execute(built.program, cpu, memory, 64);
	materialize_flags(cpu);
	return expect(result.event == uml::cpu::EngineEvent::system_call &&
			result.retired_instructions == 5 && cpu.rip == 16,
			"glibc rwlock XADD sequence did not reach SYSCALL") &&
		expect(memory.dword(lock) == 8 &&
			cpu.gpr(Register::rax) == 8,
			"glibc rwlock XADD returned or stored the wrong state") &&
		expect((cpu.rflags & (rflags_sign | rflags_zero)) == 0,
			"glibc rwlock XADD produced an impossible failure state");
}

/* GObject stores closure state in one packed dword. Its invalidation path
 * clears the eight-bit notifier count with this compiler-generated sequence
 * before a dword CMPXCHG. Pressure-vessel exposed an impossible count in this
 * exact loop, so retain the complete register-width and immediate semantics.
 */
[[nodiscard]] bool gobject_closure_bitfield_cmpxchg_is_exact()
{
	using namespace uml::cpu::x86_64;
	constexpr std::uint64_t closure = 0x800;
	constexpr std::uint64_t stack = 0x1000;
	constexpr std::uint32_t initial = 0x8808'0002;
	constexpr std::uint32_t expected = 0x8800'0002;
	TestMemory memory;
	memory.load(0, {
		0x8b, 0x03,                         // mov eax,[rbx]
		0x89, 0x44, 0x24, 0x40,             // mov [rsp+0x40],eax
		0x48, 0x8b, 0x54, 0x24, 0x40,       // mov rdx,[rsp+0x40]
		0x48, 0x8b, 0x4c, 0x24, 0x40,       // mov rcx,[rsp+0x40]
		0x48, 0xc1, 0xea, 0x13,             // shr rdx,19
		0x48, 0x81, 0xe1, 0xff, 0xff, 0x07, 0xf8,
		0x83, 0xea, 0x01,                   // sub edx,1
		0x0f, 0xb6, 0xd2,                   // movzx edx,dl
		0x48, 0xc1, 0xe2, 0x13,             // shl rdx,19
		0x48, 0x09, 0xd1,                   // or rcx,rdx
		0x48, 0x89, 0x4c, 0x24, 0x40,       // mov [rsp+0x40],rcx
		0x48, 0x8b, 0x4c, 0x24, 0x40,       // mov rcx,[rsp+0x40]
		0x8b, 0x54, 0x24, 0x40,             // mov edx,[rsp+0x40]
		0x48, 0xc1, 0xe9, 0x13,             // shr rcx,19
		0x0f, 0xb6, 0xc9,                   // movzx ecx,cl
		0xf0, 0x0f, 0xb1, 0x13,             // lock cmpxchg [rbx],edx
		0x75, 0x01,                         // jne failure
		0xcc,                               // success: int3
		0x0f, 0x0b,                         // failure: ud2
	});
	const std::array<std::uint32_t, 1> closure_word{initial};
	if (!expect(memory.write(closure,
			std::as_bytes(std::span{closure_word})),
		"GObject closure state did not fit test memory"))
		return false;
	const auto built = gadgets::build(memory, 0);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"GObject closure CMPXCHG sequence did not build"))
		return false;
	CpuState cpu{};
	cpu.gpr(Register::rbx) = closure;
	cpu.gpr(Register::rsp) = stack;
	const auto result = gadgets::execute(built.program, cpu, memory, 64);
	const auto deferred = cpu.deferred_flags;
	materialize_flags(cpu);
	return expect(result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 17 && cpu.rip == 67,
			"GObject closure CMPXCHG took its failure branch after success") &&
		expect(memory.dword(closure) == expected,
			"GObject closure CMPXCHG corrupted the packed notifier count") &&
		expect(cpu.gpr(Register::rax) == initial,
			"GObject closure CMPXCHG changed EAX after success") &&
		expect(cpu.gpr(Register::rcx) == 0,
			"GObject closure bitfield extraction produced the wrong count") &&
		expect(cpu.gpr(Register::rdx) == expected,
			"GObject closure bitfield update produced the wrong source") &&
		expect(deferred.operation == DeferredFlagOperation::subtract,
			"GObject closure CMPXCHG recorded the wrong flag operation") &&
		expect(deferred.left == initial,
			"GObject closure CMPXCHG recorded the wrong expected value") &&
		expect(deferred.right == initial,
			"GObject closure CMPXCHG recorded the wrong observed value") &&
		expect(deferred.result == 0,
			"GObject closure CMPXCHG recorded a nonzero comparison") &&
		expect(deferred.width == 4,
			"GObject closure CMPXCHG recorded the wrong flag width") &&
		expect((cpu.rflags & rflags_zero) != 0,
			"GObject closure CMPXCHG lost its success flag");
}

/* XADD is implicitly atomic against memory even without relying on a distinct
 * LOCK-prefixed gadget. Exercise the same aligned dword operation from the
 * host threads that represent concurrent guest vCPUs.
 */
[[nodiscard]] bool xadd_survives_concurrent_vcpus()
{
	using namespace uml::cpu::x86_64;
	constexpr unsigned thread_count = 4;
	constexpr unsigned iterations = 10000;
	constexpr std::uint64_t counter = 0x800;
	TestMemory memory;
	memory.load(0, {0xf0, 0x0f, 0xc1, 0x03, 0x0f, 0x05});
	const auto built = gadgets::build(memory, 0);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"contended XADD block did not build"))
		return false;
	std::atomic<bool> healthy{true};
	std::vector<std::thread> threads;
	threads.reserve(thread_count);
	for (unsigned index = 0; index < thread_count; ++index) {
		threads.emplace_back([&] {
			CpuState cpu{};
			cpu.gpr(Register::rbx) = counter;
			for (unsigned step = 0; step < iterations; ++step) {
				cpu.rip = 0;
				cpu.gpr(Register::rax) = 8;
				const auto result = gadgets::execute(
					built.program, cpu, memory, 64);
				if (result.event != uml::cpu::EngineEvent::system_call) {
					healthy.store(false, std::memory_order_relaxed);
					return;
				}
			}
		});
	}
	for (auto &thread : threads)
		thread.join();
	return expect(healthy.load(std::memory_order_relaxed),
		"a contended XADD thread left the gadget stream early") &&
		expect(memory.dword(counter) ==
			thread_count * iterations * 8,
			"concurrent XADD lost updates");
}

/* Model the compare/exchange retry used by glibc and libstdc++ locks. A failed
 * CMPXCHG supplies the newly observed value through EAX; the next attempt uses
 * it without reading shared memory non-atomically in the test harness.
 */
[[nodiscard]] bool cmpxchg_survives_concurrent_vcpus()
{
	using namespace uml::cpu::x86_64;
	constexpr unsigned thread_count = 4;
	constexpr unsigned iterations = 5000;
	constexpr unsigned retry_limit = 10000;
	constexpr std::uint64_t counter = 0x800;
	TestMemory memory;
	memory.load(0, {0xf0, 0x0f, 0xb1, 0x0b, 0x0f, 0x05});
	const auto built = gadgets::build(memory, 0);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"contended CMPXCHG block did not build"))
		return false;
	std::atomic<bool> healthy{true};
	std::vector<std::thread> threads;
	threads.reserve(thread_count);
	for (unsigned index = 0; index < thread_count; ++index) {
		threads.emplace_back([&] {
			CpuState cpu{};
			cpu.gpr(Register::rbx) = counter;
			std::uint32_t expected = 0;
			for (unsigned step = 0; step < iterations; ++step) {
				bool exchanged = false;
				for (unsigned retry = 0;
				     retry < retry_limit &&
					 healthy.load(std::memory_order_relaxed); ++retry) {
					const auto desired = expected + 1;
					cpu.rip = 0;
					cpu.gpr(Register::rax) = expected;
					cpu.gpr(Register::rcx) = desired;
					const auto result = gadgets::execute(
						built.program, cpu, memory, 64);
					if (result.event != uml::cpu::EngineEvent::system_call) {
						healthy.store(false, std::memory_order_relaxed);
						return;
					}
					materialize_flags(cpu);
					if ((cpu.rflags & rflags_zero) != 0) {
						expected = desired;
						exchanged = true;
						break;
					}
					expected = static_cast<std::uint32_t>(
						cpu.gpr(Register::rax));
				}
				if (!exchanged) {
					healthy.store(false, std::memory_order_relaxed);
					return;
				}
			}
		});
	}
	for (auto &thread : threads)
		thread.join();
	return expect(healthy.load(std::memory_order_relaxed),
		"a contended CMPXCHG retry did not make progress") &&
		expect(memory.dword(counter) == thread_count * iterations,
			"concurrent CMPXCHG lost successful updates");
}

/* vCPU threads execute guest code concurrently, so a guest LOCK update has to
 * stay atomic against the host threads running the other vCPUs.
 */
[[nodiscard]] bool locked_increment_survives_concurrent_vcpus()
{
	using namespace uml::cpu::x86_64;
	constexpr unsigned thread_count = 4;
	constexpr unsigned iterations = 20000;
	constexpr std::uint64_t counter = 0x800;
	TestMemory memory;
	memory.load(0, {0xf0, 0xff, 0x00, 0x0f, 0x05});
	const auto built = gadgets::build(memory, 0);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"locked increment block did not build"))
		return false;
	std::atomic<bool> healthy{true};
	std::vector<std::thread> threads;
	threads.reserve(thread_count);
	for (unsigned index = 0; index < thread_count; ++index) {
		threads.emplace_back([&] {
			for (unsigned step = 0; step < iterations; ++step) {
				CpuState cpu{};
				cpu.gpr(Register::rax) = counter;
				const auto result = gadgets::execute(
					built.program, cpu, memory, 64);
				if (result.event != uml::cpu::EngineEvent::system_call) {
					healthy.store(false);
					return;
				}
			}
		});
	}
	for (auto &thread : threads)
		thread.join();
	return expect(healthy.load(),
		"a locked increment thread left the gadget stream early") &&
		expect(memory.dword(counter) == thread_count * iterations,
			"concurrent LOCK INC lost updates");
}

} // namespace

int main()
{
	const bool passed = decode_scaled_address() &&
		cache_reuses_and_revalidates_programs() &&
		cache_validates_inline_source_spans() &&
		cache_rejects_unrelated_code_pages_by_stamp() &&
		cache_retires_the_oldest_generation() &&
		cache_links_common_successors() &&
		cache_chains_direct_branches() &&
		cache_chains_indirect_transfers() &&
		an_unrelated_code_change_leaves_a_link_armed() &&
		linked_call_crosses_a_disjoint_executable_stack_write() &&
		linked_call_stops_when_the_stack_write_hits_its_target() &&
		executable_store_publishes_code_change() &&
		store_publishes_only_where_some_executor_caches_code() &&
		overlapping_executable_store_stops_immediately() &&
		executable_store_links_only_to_unchanged_code() &&
		distant_code_write_cools_its_linked_block() &&
		code_write_resets_unvalidated_successors() &&
		slow_executable_writes_publish_once_per_native_run() &&
		execute_scalar_control_flow() &&
		materialized_flags_match_the_reference() &&
		carry_branches_match_the_reference() &&
		execute_absolute_memory_test_branch() &&
		execute_lazy_zero_branches() &&
		execute_compat32_test_greater_after_syscall() &&
		execute_compat32_futex_result_validator() &&
		shifts_overlay_lazy_arithmetic_flags() &&
		lazy_shift_flags_match_the_definition() &&
		execute_segment_memory() &&
		execute_addressing_shapes() &&
		execute_byte_sign_masks() &&
		execute_call_and_return() &&
		decode_compat32_defaults() &&
		execute_compat32_stack_and_int80() &&
		execute_compat32_pushfd_popfd() &&
		execute_compat32_legacy_segment_stack() &&
		execute_long64_interrupt_return_stack_switch() &&
		expose_consistent_xsave_cpuid_state() &&
		wrap_compat32_instruction_pointer() &&
		execute_compat32_tls_segment() &&
		reject_program_mode_mismatch() &&
		chunk_repeating_string_for_interrupt() &&
		bulk_string_runs_match_the_element_definition() &&
		repeated_store_into_executable_page_publishes_its_run() &&
		repeated_move_faults_on_a_run_boundary() &&
		fused_conditions_match_the_deferred_evaluation() &&
		fused_conditions_cover_exactly_the_legal_pairs() &&
		lazy_carry_reaches_every_reader() &&
		fused_conditional_jumps_take_the_same_edge() &&
		cpuid_table_matches_the_gadget() &&
		preserve_precise_memory_fault() &&
		retire_valid_prefix_before_decode_fault() &&
		migrated_gadgets_are_native_only() &&
		native_divide_traps_precisely() &&
		native_into_reports_overflow_precisely() &&
		vex_vmovlhps_preserves_aliased_source() &&
		vector_invalid_results_use_x86_indefinite_nan() &&
		locked_memory_updates_select_exclusive_gadgets() &&
		glibc_rwlock_xadd_fast_path_is_exact() &&
		gobject_closure_bitfield_cmpxchg_is_exact() &&
		xadd_survives_concurrent_vcpus() &&
		cmpxchg_survives_concurrent_vcpus() &&
		locked_increment_survives_concurrent_vcpus();
	return passed ? 0 : 1;
}
