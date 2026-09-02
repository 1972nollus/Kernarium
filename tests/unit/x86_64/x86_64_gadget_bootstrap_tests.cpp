/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/decoder.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
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
#include <thread>
#include <vector>

namespace {

class TestMemory final : public uml::memory::GuestAddressSpace {
public:
	explicit TestMemory(std::uint64_t base = 0) noexcept : base_{base} {}

	void load(std::uint64_t address,
		std::initializer_list<std::uint8_t> bytes) noexcept
	{
		const auto first = address;
		for (const auto byte : bytes)
			bytes_[address++ - base_] = static_cast<std::byte>(byte);
		bump_code_stamps(first, bytes.size());
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
		return (address & ~std::uint64_t{4095}) == executable_page_;
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
	std::array<std::byte, 16384> bytes_{};
	uml::memory::MemoryFault fault_{};
	bool faulted_{};
	std::size_t fetches_{};
	std::size_t invalidations_{};
	std::atomic<std::size_t> atomic_pointers_{};
	bool atomic_pointer_available_{true};
	std::atomic<std::size_t> executable_queries_{};
	std::size_t code_stamp_queries_{};
	std::array<std::uint64_t, 4> code_stamp_versions_{1, 1, 1, 1};
	bool code_stamps_available_{};
	std::uint64_t executable_page_{~std::uint64_t{}};
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
	memory.load(48, {0xff, 0xd0});
	memory.load(512, {0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});

	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
	const auto target_a = executor.select_program(
		memory, 16, ExecutionMode::long64);
	const auto target_b = executor.select_program(
		memory, 24, ExecutionMode::long64);
	const auto jump = executor.select_program(
		memory, 0, ExecutionMode::long64);
	const auto ret = executor.select_program(
		memory, 32, ExecutionMode::long64);
	const auto call = executor.select_program(
		memory, 48, ExecutionMode::long64);
	if (!expect(target_a.program != nullptr && target_b.program != nullptr &&
			jump.program != nullptr && ret.program != nullptr &&
			call.program != nullptr,
		"indirect-link setup did not cache all programs"))
		return false;

	CpuState cpu{};
	cpu.gpr(Register::rax) = 16;
	const auto jump_cold_a = gadgets::execute(
		*jump.program, cpu, memory, executor, 64);
	if (!expect(jump_cold_a.event == uml::cpu::EngineEvent::continued &&
			jump_cold_a.retired_instructions == 1 && cpu.rip == 16,
		"cold indirect JMP did not stop and train its target"))
		return false;

	cpu = CpuState{};
	cpu.gpr(Register::rax) = 24;
	const auto jump_cold_b = gadgets::execute(
		*jump.program, cpu, memory, executor, 64);
	if (!expect(jump_cold_b.event == uml::cpu::EngineEvent::continued &&
			jump_cold_b.retired_instructions == 1 && cpu.rip == 24,
		"second indirect JMP target did not train the bimorphic cell"))
		return false;

	cpu = CpuState{};
	cpu.gpr(Register::rax) = 16;
	const auto jump_hot_a = gadgets::execute(
		*jump.program, cpu, memory, executor, 64);
	if (!expect(jump_hot_a.event == uml::cpu::EngineEvent::system_call &&
			jump_hot_a.retired_instructions == 3 &&
			cpu.gpr(Register::rax) == 42 && cpu.rip == 23,
		"first indirect JMP target was not retained by the two-entry cell"))
		return false;

	cpu = CpuState{};
	cpu.gpr(Register::rax) = 24;
	const auto jump_hot_b = gadgets::execute(
		*jump.program, cpu, memory, executor, 64);
	if (!expect(jump_hot_b.event == uml::cpu::EngineEvent::system_call &&
			jump_hot_b.retired_instructions == 3 &&
			cpu.gpr(Register::rax) == 43 && cpu.rip == 31,
		"second indirect JMP target was not retained by the two-entry cell"))
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
	cpu.rip = 32;
	cpu.gpr(Register::rsp) = 512;
	const auto ret_cold = gadgets::execute(
		*ret.program, cpu, memory, executor, 64);
	if (!expect(ret_cold.event == uml::cpu::EngineEvent::continued &&
			ret_cold.retired_instructions == 1 && cpu.rip == 16 &&
			cpu.gpr(Register::rsp) == 520,
		"cold RET did not update RSP and train its target"))
		return false;

	cpu = CpuState{};
	cpu.rip = 32;
	cpu.gpr(Register::rsp) = 512;
	const auto ret_hot = gadgets::execute(
		*ret.program, cpu, memory, executor, 64);
	if (!expect(ret_hot.event == uml::cpu::EngineEvent::system_call &&
			ret_hot.retired_instructions == 3 && cpu.rip == 23 &&
			cpu.gpr(Register::rsp) == 520,
		"trained RET did not enter its native successor"))
		return false;

	cpu = CpuState{};
	cpu.rip = 48;
	cpu.gpr(Register::rax) = 16;
	cpu.gpr(Register::rsp) = 1024;
	const auto call_cold = gadgets::execute(
		*call.program, cpu, memory, executor, 64);
	if (!expect(call_cold.event == uml::cpu::EngineEvent::continued &&
			call_cold.retired_instructions == 1 && cpu.rip == 16 &&
			cpu.gpr(Register::rsp) == 1016 && memory.qword(1016) == 50,
		"cold indirect CALL did not push its return and train its target"))
		return false;

	cpu = CpuState{};
	cpu.rip = 48;
	cpu.gpr(Register::rax) = 16;
	cpu.gpr(Register::rsp) = 1024;
	const auto call_hot = gadgets::execute(
		*call.program, cpu, memory, executor, 64);
	return expect(call_hot.event == uml::cpu::EngineEvent::system_call &&
			call_hot.retired_instructions == 3 && cpu.rip == 23,
		"trained indirect CALL did not enter its native successor") &&
		expect(cpu.gpr(Register::rsp) == 1016 &&
			memory.qword(1016) == 50,
			"trained indirect CALL lost its architectural stack write");
}

[[nodiscard]] bool linked_call_stops_after_executable_stack_write()
{
	using namespace uml::cpu::x86_64;
	TestMemory memory;
	memory.load(0, {0xe8, 0x0b, 0x00, 0x00, 0x00});
	memory.load(16, {0x0f, 0x05});
	memory.set_executable_page(0);
	gadgets::Executor executor;
	executor.prepare(memory, 42, 1, 1);
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
	return expect(result.event == uml::cpu::EngineEvent::continued &&
			result.retired_instructions == 1 && cpu.rip == 16,
		"direct CALL crossed a code-changing stack write") &&
		expect(cpu.gpr(Register::rsp) == 0x7f8 &&
			memory.qword(0x7f8) == 5 && memory.invalidations() == 1,
			"code-changing CALL lost its architectural stack write");
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
	const auto built = gadgets::build(memory, 0);
	if (!expect(built.status == gadgets::BuildStatus::built,
		"executable-store block did not build"))
		return false;
	CpuState cpu{};
	cpu.gpr(Register::rax) = 0x800;
	gadgets::Executor executor;
	executor.prepare(memory, 1, 1, 1);
	const auto result = gadgets::execute(
		built.program, cpu, memory, executor, 64);
	return expect(result.event == uml::cpu::EngineEvent::system_call &&
		result.retired_instructions == 3 && cpu.rip == 9,
		"non-overlapping executable stores did not stay in one native block") &&
		expect(memory.executable_queries() == 1,
			"write-TLB executable tag was redundantly revalidated") &&
		expect(memory.invalidations() == 1,
			"batched executable stores did not publish one code change") &&
		expect(memory.byte(0x800) == 0x7f && memory.byte(0x801) == 0x6e,
			"batched executable stores did not update guest memory");
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
	return expect(branch_result.event == uml::cpu::EngineEvent::continued &&
			branch_result.retired_instructions == 3 && cpu.rip == 12,
		"JE did not consume the deferred shift zero override") &&
		expect(cpu.deferred_flags.operation == DeferredFlagOperation::add &&
			(cpu.deferred_flags.override_mask & rflags_zero) != 0 &&
			(cpu.deferred_flags.override_value & rflags_zero) != 0,
			"zero-branch fast path eagerly materialized the shift overlay");
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
	TestMemory memory;
	memory.load(0, {0xf3, 0xaa}); // rep stosb
	CpuState cpu{};
	cpu.gpr(Register::rax) = 0x5a;
	cpu.gpr(Register::rcx) = 4097;
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
			cpu.gpr(Register::rdi) == 8192 &&
			memory.byte(4096) == 0x5a && memory.byte(8191) == 0x5a,
			"REP STOSB chunk did not preserve its partial state"))
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
			cpu.gpr(Register::rdi) == 8193 && memory.byte(8192) == 0x5a,
			"REP STOSB did not complete from its partial state");
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
		/* An unprefixed update keeps the cheaper non-exclusive gadget. */
		expect(first_native_entry({0xff, 0x00, 0x0f, 0x05}) ==
			gadget_entry(native::uml_x86_64_gadget_increment_32_m),
			"unlocked INC lost its plain gadget") &&
		expect(first_native_entry({0x01, 0x08, 0x0f, 0x05}) ==
			gadget_entry(native::uml_x86_64_gadget_add_32_mr),
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
		linked_call_stops_after_executable_stack_write() &&
		executable_store_publishes_code_change() &&
		overlapping_executable_store_stops_immediately() &&
		executable_store_links_only_to_unchanged_code() &&
		code_write_resets_unvalidated_successors() &&
		slow_executable_writes_publish_once_per_native_run() &&
		execute_scalar_control_flow() &&
		execute_absolute_memory_test_branch() &&
		execute_lazy_zero_branches() &&
		execute_compat32_test_greater_after_syscall() &&
		execute_compat32_futex_result_validator() &&
		shifts_overlay_lazy_arithmetic_flags() &&
		execute_segment_memory() &&
		execute_call_and_return() &&
		decode_compat32_defaults() &&
		execute_compat32_stack_and_int80() &&
		execute_compat32_legacy_segment_stack() &&
		execute_long64_interrupt_return_stack_switch() &&
		expose_consistent_xsave_cpuid_state() &&
		wrap_compat32_instruction_pointer() &&
		execute_compat32_tls_segment() &&
		reject_program_mode_mismatch() &&
		chunk_repeating_string_for_interrupt() &&
		preserve_precise_memory_fault() &&
		retire_valid_prefix_before_decode_fault() &&
		migrated_gadgets_are_native_only() &&
		native_divide_traps_precisely() &&
		native_into_reports_overflow_precisely() &&
		locked_memory_updates_select_exclusive_gadgets() &&
		glibc_rwlock_xadd_fast_path_is_exact() &&
		gobject_closure_bitfield_cmpxchg_is_exact() &&
		xadd_survives_concurrent_vcpus() &&
		cmpxchg_survives_concurrent_vcpus() &&
		locked_increment_survives_concurrent_vcpus();
	return passed ? 0 : 1;
}
