// SPDX-License-Identifier: GPL-2.0-or-later
#include "gadget_executor.hpp"

#include "cpu/arm64/dispatcher.hpp"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

namespace uml::backend {
namespace {

constexpr std::size_t block_instruction_count = 64;
constexpr std::size_t guest_page_size = DARWIN_UML_GUEST_PAGE_SIZE;

[[nodiscard]] std::size_t fetch_instruction_window(
	StaticGuestMemory &memory, std::uint64_t pc,
	std::span<std::uint32_t> instructions)
{
	std::size_t count = 0;
	auto bytes = std::as_writable_bytes(instructions);
	while (count < instructions.size()) {
		const auto address = pc + count * sizeof(std::uint32_t);
		const auto page_remaining =
			guest_page_size - (address & (guest_page_size - 1));
		auto page_instructions = std::min<std::size_t>(
			instructions.size() - count,
			static_cast<std::size_t>(
				page_remaining / sizeof(std::uint32_t)));
		/* A valid A64 PC is word aligned. Preserve deterministic fault
		 * handling for a malformed PC instead of constructing an empty span.
		 */
		if (page_instructions == 0)
			page_instructions = 1;
		const auto offset = count * sizeof(std::uint32_t);
		const auto length = page_instructions * sizeof(std::uint32_t);
		if (!memory.fetch(address, bytes.subspan(offset, length)))
			break;
		count += page_instructions;
	}
	return count;
}

const bool trace_blocks_enabled = [] {
	const auto *value = std::getenv("UML_STATIC_TRACE_BLOCKS");
	return value != nullptr && value[0] != '\0' && value[0] != '0';
}();

const bool block_links_enabled = [] {
	const auto *value = std::getenv("UML_STATIC_DISABLE_BLOCK_LINKS");
	return value == nullptr || value[0] == '\0' || value[0] == '0';
}();

[[nodiscard]] bool verification_enabled() noexcept
{
	static const bool enabled = [] {
		const auto *value = std::getenv("UML_STATIC_VERIFY_GADGETS");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();
	return enabled;
}

void trace_block(std::uint64_t pc, const arm64::gadgets::BuildResult &built,
		 arm64::gadgets::Event event) noexcept
{
	static thread_local unsigned emitted = 0;
	if (emitted >= 4096)
		return;
	++emitted;
	std::fprintf(stderr,
		"Darwin static block pc=%#llx count=%zu complete=%u stop=%#llx/%#x event=%d\n",
		static_cast<unsigned long long>(pc),
		built.program.instruction_count(), built.complete ? 1U : 0U,
		static_cast<unsigned long long>(built.stop_address),
		built.stop_instruction, static_cast<int>(event));
}

class OverlayMemory final : public arm64::GuestMemory {
public:
	explicit OverlayMemory(StaticGuestMemory &memory) : memory_{memory} {}

	bool read(std::uint64_t address,
		  std::span<std::byte> destination) override
	{
		if (!memory_.read(address, destination))
			return false;
		apply(address, destination);
		return true;
	}

	bool fetch(std::uint64_t address,
		   std::span<std::byte> destination) override
	{
		if (!memory_.fetch(address, destination))
			return false;
		apply(address, destination);
		return true;
	}

	bool write(std::uint64_t address,
		   std::span<const std::byte> source) override
	{
		if (!memory_.validate_write(address, source.size()))
			return false;
		for (std::size_t index = 0; index < source.size(); ++index)
			writes_.insert_or_assign(address + index, source[index]);
		return true;
	}

	arm64::MemoryFault last_fault() const noexcept override
	{
		return memory_.last_fault();
	}

	[[nodiscard]] bool matches_guest() const
	{
		for (const auto &[address, expected] : writes_) {
			std::byte actual{};
			if (!memory_.read(address, std::span{&actual, 1}) ||
			    actual != expected) {
				std::fprintf(stderr,
					"Darwin static verify memory mismatch address=%#llx expected=%#x actual=%#x\n",
					static_cast<unsigned long long>(address),
					std::to_integer<unsigned>(expected),
					std::to_integer<unsigned>(actual));
				return false;
			}
		}
		return true;
	}

private:
	void apply(std::uint64_t address, std::span<std::byte> destination) const
	{
		for (std::size_t index = 0; index < destination.size(); ++index)
			if (const auto found = writes_.find(address + index);
			    found != writes_.end())
				destination[index] = found->second;
	}

	StaticGuestMemory &memory_;
	std::unordered_map<std::uint64_t, std::byte> writes_;
};

[[nodiscard]] bool same_cpu(const arm64::CpuState &expected,
			    const arm64::CpuState &actual, std::uint64_t pc)
{
	bool same = true;
	for (std::size_t index = 0; index < expected.registers.size(); ++index) {
		if (expected.registers[index] == actual.registers[index])
			continue;
		std::fprintf(stderr,
			"Darwin static verify register mismatch block=%#llx x%zu=%#llx/%#llx\n",
			static_cast<unsigned long long>(pc), index,
			static_cast<unsigned long long>(expected.registers[index]),
			static_cast<unsigned long long>(actual.registers[index]));
		same = false;
	}
	if (expected.sp != actual.sp || expected.pc != actual.pc ||
	    expected.nzcv != actual.nzcv || expected.tls != actual.tls ||
	    std::memcmp(expected.vectors.data(), actual.vectors.data(),
			sizeof(expected.vectors)) != 0 ||
	    expected.fpcr != actual.fpcr || expected.fpsr != actual.fpsr) {
		std::fprintf(stderr,
			"Darwin static verify state mismatch block=%#llx sp=%#llx/%#llx pc=%#llx/%#llx nzcv=%#x/%#x tls=%#llx/%#llx\n",
			static_cast<unsigned long long>(pc),
			static_cast<unsigned long long>(expected.sp),
			static_cast<unsigned long long>(actual.sp),
			static_cast<unsigned long long>(expected.pc),
			static_cast<unsigned long long>(actual.pc), expected.nzcv,
			actual.nzcv, static_cast<unsigned long long>(expected.tls),
			static_cast<unsigned long long>(actual.tls));
		same = false;
	}
	return same;
}

} // namespace

extern "C" void uml_arm64_gadget_link_exit();

class GadgetExecutor::ProgramCache final {
public:
	ProgramCache()
	{
		unlinked_stream_[0] =
			reinterpret_cast<std::uintptr_t>(uml_arm64_gadget_link_exit);
	}

	void invalidate() noexcept
	{
		links_.clear();
		++epoch_;
		if (epoch_ != 0)
			return;
		for (auto &slab : slabs_)
			if (slab)
				for (auto &slot : slab->slots)
					slot.epoch = 0;
		epoch_ = 1;
	}

	[[nodiscard]] const BuildResult *find(std::uint64_t pc) const noexcept
	{
		const auto location = locate(pc);
		const auto &slab = slabs_[location.slab];
		if (!slab)
			return nullptr;
		for (std::size_t way = 0; way < ways; ++way) {
			const auto &slot = slab->slots[location.offset + way];
			if (slot.epoch == epoch_ && slot.pc == pc)
				return &*slot.result;
		}
		return nullptr;
	}

	[[nodiscard]] const arm64::gadgets::BlockLink *link(
		std::uint64_t pc)
	{
		const auto found = links_.find(pc);
		if (found != links_.end())
			return found->second.get();
		auto created = std::make_unique<arm64::gadgets::BlockLink>(
			arm64::gadgets::BlockLink{
				.stream = unlinked_stream_.data(),
				.instruction_count = 0,
				.guest_pc = pc,
			});
		const auto *result = created.get();
		links_.emplace(pc, std::move(created));
		return result;
	}

	[[nodiscard]] const BuildResult &store(
		std::uint64_t pc, BuildResult result)
	{
		const auto location = locate(pc);
		auto &slab = slabs_[location.slab];
		if (!slab)
			slab = std::make_unique<Slab>();

		auto &replacement = next_replacement_[location.set];
		std::size_t selected = replacement;
		for (std::size_t way = 0; way < ways; ++way) {
			const auto &slot = slab->slots[location.offset + way];
			if (slot.epoch != epoch_) {
				selected = way;
				break;
			}
		}
		replacement = static_cast<std::uint8_t>((selected + 1) & (ways - 1));

		auto &slot = slab->slots[location.offset + selected];
		if (slot.epoch == epoch_ && slot.result)
			deactivate(slot.pc, slot.result->program);
		slot.result.emplace(std::move(result));
		slot.pc = pc;
		slot.epoch = epoch_;
		activate(pc, *slot.result);
		return *slot.result;
	}

	[[nodiscard]] std::pmr::memory_resource *program_storage() noexcept
	{
		return &program_storage_;
	}

private:
	static constexpr std::size_t ways = 4;
	static constexpr std::size_t set_count = 4 * 1024;
	static constexpr std::size_t slot_count = ways * set_count;
	static constexpr std::size_t slab_slots = 256;
	static constexpr std::size_t slab_count = slot_count / slab_slots;

	struct Slot final {
		std::optional<BuildResult> result;
		std::uint64_t pc{};
		std::uint64_t epoch{};
	};

	struct Slab final {
		std::array<Slot, slab_slots> slots;
	};

	struct Location final {
		std::size_t set;
		std::size_t slab;
		std::size_t offset;
	};

	[[nodiscard]] static Location locate(std::uint64_t pc) noexcept
	{
		const auto block = pc >> 2;
		const auto mixed = block ^ (block >> 12) ^ (block >> 24);
		const auto set = static_cast<std::size_t>(mixed) & (set_count - 1);
		const auto first_slot = set * ways;
		return {
			.set = set,
			.slab = first_slot / slab_slots,
			.offset = first_slot % slab_slots,
		};
	}

	void activate(std::uint64_t pc, const BuildResult &result)
	{
		if (!result.complete || result.program.instruction_count() == 0 ||
		    result.program.accesses_fp_state())
			return;
		auto *target = const_cast<arm64::gadgets::BlockLink *>(link(pc));
		target->stream = result.program.words().data();
		target->instruction_count = result.program.instruction_count();
	}

	void deactivate(std::uint64_t pc,
			const arm64::gadgets::Program &program) noexcept
	{
		const auto found = links_.find(pc);
		if (found == links_.end() ||
		    found->second->stream != program.words().data())
			return;
		found->second->stream = unlinked_stream_.data();
		found->second->instruction_count = 0;
	}

	std::array<std::uintptr_t, 1> unlinked_stream_{};
	std::pmr::unsynchronized_pool_resource program_storage_;
	std::unordered_map<std::uint64_t,
		std::unique_ptr<arm64::gadgets::BlockLink>> links_;
	std::array<std::unique_ptr<Slab>, slab_count> slabs_;
	std::array<std::uint8_t, set_count> next_replacement_{};
	std::uint64_t epoch_{1};
};

GadgetExecutor::GadgetExecutor() : programs_{std::make_unique<ProgramCache>()}
{
}

GadgetExecutor::~GadgetExecutor()
{
	darwin_uml_static_vm_execution_context_destroy(execution_context_);
}

bool GadgetExecutor::begin_execution(unsigned long generation,
				     std::uint64_t guest_pc)
{
	preempted_ = false;
	auto *context = execution_context(generation);
	return context != nullptr &&
		darwin_uml_static_vm_execution_context_begin(context, guest_pc) == 0;
}

void GadgetExecutor::end_execution() noexcept
{
	if (execution_context_ != nullptr)
		(void)darwin_uml_static_vm_execution_context_end(execution_context_);
}

void *GadgetExecutor::execution_context(unsigned long generation)
{
	if (execution_context_ != nullptr && execution_generation_ == generation)
		return execution_context_;
	darwin_uml_static_vm_execution_context_destroy(execution_context_);
	execution_context_ =
		darwin_uml_static_vm_execution_context_create(generation);
	execution_generation_ = execution_context_ != nullptr ? generation : 0;
	return execution_context_;
}

void GadgetExecutor::prepare_cache(
	unsigned long generation, std::uint64_t changes)
{
	if (cache_generation_ == generation && cache_changes_ == changes)
		return;
	programs_->invalidate();
	cache_generation_ = generation;
	cache_changes_ = changes;
}

std::optional<GadgetExecutor::CachedResult> GadgetExecutor::find(
	std::uint64_t pc) const
{
	const auto *found = programs_->find(pc);
	if (found == nullptr)
		return std::nullopt;
	return std::cref(*found);
}

const GadgetExecutor::BuildResult &GadgetExecutor::store(
	std::uint64_t pc, BuildResult result)
{
	return programs_->store(pc, std::move(result));
}

arm64::gadgets::Event GadgetExecutor::run_block(
	arm64::CpuState &cpu, StaticGuestMemory &memory, unsigned long generation,
	std::size_t &executed)
{
	executed = 0;
	auto *context = execution_context(generation);
	if (context == nullptr)
		return arm64::gadgets::Event::fatal;
	std::array<std::uint32_t, block_instruction_count> instructions;
	bool instructions_loaded = false;
	const bool verification_requested = verification_enabled();
	const auto sequences =
		darwin_uml_static_vm_execution_context_sequences(context);
	preempted_ = sequences.execution_interrupt != 0;
	if (preempted_)
		return arm64::gadgets::Event::interrupt;
	if (mapping_generation_ != generation ||
	    mapping_changes_ != sequences.mapping_changes) {
		memory.prepare(generation, sequences.mapping_changes);
		tlb_.prepare(generation, sequences.mapping_changes);
		mapping_generation_ = generation;
		mapping_changes_ = sequences.mapping_changes;
	}
	prepare_cache(generation, sequences.code_changes);
	std::optional<CachedResult> selected;
	std::optional<BuildResult> uncached;
	if (!verification_requested)
		selected = find(cpu.pc);
	if (!selected) {
		const auto count = fetch_instruction_window(
			memory, cpu.pc, instructions);
		if (count == 0) {
			const auto fault = memory.last_fault();
			tlb_.fault_address = fault.address;
			tlb_.fault_access = fault.access == arm64::MemoryAccess::write ?
				arm64::gadgets::Access::write :
				arm64::gadgets::Access::read;
			return arm64::gadgets::Event::memory_fault;
		}
		instructions_loaded = true;
		auto decoded = [&] {
			const auto window = std::span{instructions}.first(count);
			if (verification_requested)
				return arm64::gadgets::Builder{}.build(cpu.pc, window);
			if (!block_links_enabled)
				return arm64::gadgets::Builder{}.build(
					cpu.pc, window, {},
					programs_->program_storage());
			return arm64::gadgets::Builder{}.build(
				cpu.pc, window,
				[this](std::uint64_t pc) {
					return programs_->link(pc);
				},
				programs_->program_storage());
		}();
		if (verification_requested) {
			uncached.emplace(std::move(decoded));
			selected = std::cref(*uncached);
		} else {
			selected = std::cref(store(cpu.pc, std::move(decoded)));
		}
	}
	const auto &built = selected->get();
	if (!built.complete || built.program.instruction_count() == 0) {
		std::fprintf(stderr,
			"Darwin static unsupported instruction pc=%#llx value=%#x compiled=%zu\n",
			static_cast<unsigned long long>(built.stop_address),
			built.stop_instruction,
			built.program.instruction_count());
		if (trace_blocks_enabled)
			trace_block(cpu.pc, built, arm64::gadgets::Event::fatal);
		cpu.pc = built.stop_address;
		return arm64::gadgets::Event::fatal;
	}
	/* The reference overlay intentionally does not expose direct atomic
	 * pointers because a speculative reference store must not modify live
	 * guest RAM.  Its synthetic reservation keys therefore cannot be compared
	 * with a reservation established by the live TLB in an earlier block.
	 * Leave exclusive blocks on the real dispatcher path and keep verifying
	 * every surrounding block; otherwise the verifier reports a false mismatch
	 * at ordinary LDXR/STXR retry loops before userspace starts.
	 */
	const bool verifying = verification_requested && instructions_loaded &&
		std::none_of(instructions.begin(),
			instructions.begin() + built.program.instruction_count(),
			[](std::uint32_t instruction) {
				return (instruction & 0x3f00'0000U) == 0x0800'0000U;
			});
	std::unique_ptr<arm64::CpuState> expected;
	arm64::RunResult expected_result{};
	std::unique_ptr<OverlayMemory> overlay;
	if (verifying) {
		/* The reference dispatcher accesses vectors directly.  Materialize the
		 * deferred kernel frame before copying it so gadget verification observes
		 * the same incoming FP state as normal execution.
		 */
		if (built.program.accesses_fp_state())
			cpu.access_fp_state();
		expected = std::make_unique<arm64::CpuState>(cpu);
		overlay = std::make_unique<OverlayMemory>(memory);
		expected_result = arm64::Dispatcher{}.run(
			*expected, *overlay, built.program.instruction_count());
	}
	const auto event =
		arm64::gadgets::execute(built.program, cpu, tlb_, executed);
	if (event == arm64::gadgets::Event::fatal) {
		std::array<std::byte, sizeof(std::uint32_t)> current_encoded{};
		std::uint32_t current_instruction = 0;
		const bool current_fetched = memory.fetch(cpu.pc, current_encoded);
		if (current_fetched)
			std::memcpy(&current_instruction, current_encoded.data(),
				current_encoded.size());
		std::fprintf(stderr,
			"Darwin static fatal block=%#llx current-pc=%#llx current-instruction=%#x fetched=%u count=%zu\n",
			static_cast<unsigned long long>(built.program.guest_address()),
			static_cast<unsigned long long>(cpu.pc),
			current_instruction, current_fetched ? 1U : 0U,
			built.program.instruction_count());
		for (std::size_t index = 0;
		     index < built.program.instruction_count(); ++index) {
			const auto address = built.program.guest_address() + index * 4;
			std::uint32_t instruction = 0;
			if (instructions_loaded) {
				instruction = instructions[index];
			} else {
				std::array<std::byte, sizeof(instruction)> encoded{};
				if (memory.fetch(address, encoded))
					std::memcpy(&instruction, encoded.data(),
						    encoded.size());
			}
			std::fprintf(stderr,
				"Darwin static fatal instruction pc=%#llx value=%#x\n",
				static_cast<unsigned long long>(address), instruction);
		}
	}
	if (overlay && (!same_cpu(*expected, cpu, built.program.guest_address()) ||
			!overlay->matches_guest())) {
		std::fprintf(stderr,
			"Darwin static verify failed pc=%#llx count=%zu event=%d reference=%u/%#llx/%#x\n",
			static_cast<unsigned long long>(built.program.guest_address()),
			built.program.instruction_count(), static_cast<int>(event),
			static_cast<unsigned>(expected_result.reason),
			static_cast<unsigned long long>(expected_result.address),
			expected_result.instruction);
		for (std::size_t index = 0;
		     index < built.program.instruction_count(); ++index)
			std::fprintf(stderr,
				"Darwin static verify instruction pc=%#llx value=%#x\n",
				static_cast<unsigned long long>(
					built.program.guest_address() + index * 4),
				instructions[index]);
		std::abort();
	}
	/* Keep disabled tracing out of both the call and thread-local paths.  This
	 * boundary executes after every translated block.
	 */
	if (trace_blocks_enabled)
		trace_block(built.program.guest_address(), built, event);
	return event;
}

std::uint64_t GadgetExecutor::fault_address() const noexcept
{
	return tlb_.fault_address;
}

arm64::gadgets::Access GadgetExecutor::fault_access() const noexcept
{
	return tlb_.fault_access;
}

} // namespace uml::backend
