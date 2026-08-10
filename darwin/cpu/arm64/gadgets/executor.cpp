// SPDX-License-Identifier: GPL-2.0-or-later
#include "executor.hpp"

#include "cpu/arm64/detail/floating_exception.hpp"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

namespace uml::cpu::arm64::gadgets {
using ::uml::memory::StaticGuestMemory;
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

[[nodiscard]] constexpr bool terminal_does_not_retire(
	::uml::cpu::EngineEvent event) noexcept
{
	switch (event) {
	case ::uml::cpu::EngineEvent::fatal:
	case ::uml::cpu::EngineEvent::memory_fault:
	case ::uml::cpu::EngineEvent::illegal_instruction:
	case ::uml::cpu::EngineEvent::gcs_fault:
	case ::uml::cpu::EngineEvent::floating_point_trap:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] bool program_contains_stream(
	const arm64::gadgets::Program &program, const void *stream) noexcept
{
	if (stream == nullptr || program.words().empty())
		return false;
	const auto marker = reinterpret_cast<std::uintptr_t>(stream);
	const auto begin = reinterpret_cast<std::uintptr_t>(
		program.words().data());
	const auto end =
		begin + program.words().size() * sizeof(std::uintptr_t);
	return marker >= begin && marker < end;
}

[[nodiscard]] std::uint32_t effective_fpcr(
	const arm64::CpuState &cpu) noexcept
{
	return cpu.fp_loaded ? cpu.fpcr : cpu.deferred_fpcr;
}

void correct_terminal_instruction_count(
	const arm64::gadgets::Program &program, ::uml::cpu::EngineEvent event,
	std::uint64_t fault_pc, std::size_t &executed) noexcept
{
	if (!terminal_does_not_retire(event))
		return;

	const auto base = program.guest_address();
	if (fault_pc < base)
		return;
	const auto offset = fault_pc - base;
	if ((offset & (sizeof(std::uint32_t) - 1U)) != 0U)
		return;
	const auto fault_index = offset / sizeof(std::uint32_t);
	if (fault_index >= program.instruction_count())
		return;
	const auto unretired =
		program.instruction_count() - static_cast<std::size_t>(fault_index);
	if (executed < unretired)
		return;

	/* The assembly engine precharges a block's full translated length.  A
	 * non-retiring terminal stores its own PC, allowing the cold C++ boundary
	 * to subtract the current block's unexecuted suffix. Counts accumulated
	 * before a linked successor are retained.
	 */
	executed -= unretired;
}

void correct_fpcr_enable_instruction_count(
	const arm64::gadgets::Program &program, ::uml::cpu::EngineEvent event,
	const arm64::CpuState &cpu, std::size_t &executed) noexcept
{
	if (event != ::uml::cpu::EngineEvent::continued ||
	    (effective_fpcr(cpu) &
	     arm64::detail::fpcr_exception_enable_mask) == 0U)
		return;

	const auto base = program.guest_address();
	if (cpu.pc <= base)
		return;
	const auto offset = cpu.pc - base;
	if ((offset & (sizeof(std::uint32_t) - 1U)) != 0U)
		return;
	const auto retired = offset / sizeof(std::uint32_t);
	if (retired > program.instruction_count())
		return;
	const auto unretired =
		program.instruction_count() - static_cast<std::size_t>(retired);
	if (executed < unretired)
		return;

	/* An FPCR write that newly enables traps exits at its translated
	 * fallthrough PC. It retires, but later words in the precharged block do
	 * not. Unresolved control-flow exits cannot satisfy the enabled-state
	 * condition because enabled entries use the one-instruction cold path.
	 */
	executed -= unretired;
}

} // namespace

extern "C" void uml_arm64_gadget_link_exit();
extern "C" void uml_arm64_gadget_fp_link_entry();

class GadgetExecutor::ProgramCache final {
public:
	ProgramCache()
		: stream_index_{std::make_unique<StreamIndexSet[]>(
			  stream_index_set_count)}
	{}

	void invalidate() noexcept
	{
		indirect_links_.clear();
		links_.clear();
		++epoch_;
		if (epoch_ != 0)
			return;
		for (auto &slab : slabs_)
			if (slab)
				for (auto &slot : slab->slots)
					slot.epoch = 0;
		for (std::size_t set = 0; set < stream_index_set_count; ++set)
			for (auto &entry : stream_index_[set].entries)
				entry = {};
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

	[[nodiscard]] const BuildResult *find_stream(
		const void *stream) const noexcept
	{
		if (stream == nullptr)
			return nullptr;
		const auto marker = reinterpret_cast<std::uintptr_t>(stream);
		const auto &set = stream_index_[stream_index_location(marker)];
		for (const auto &entry : set.entries) {
			const auto *slot = entry.slot;
			if (slot != nullptr && slot->epoch == epoch_ &&
			    slot->activation == entry.activation &&
			    slot->result &&
			    program_contains_stream(slot->result->program, stream))
				return &*slot->result;
		}

		/*
		 * The fixed index deliberately permits replacement under unusual
		 * host-address collisions. Preserve exact terminal accounting with
		 * a cold correctness fallback; ordinary linked exits find one of
		 * four allocation-free entries above.
		 */
		for (const auto &slab : slabs_) {
			if (!slab)
				continue;
			for (const auto &slot : slab->slots)
				if (slot.epoch == epoch_ && slot.result &&
				    program_contains_stream(
					    slot.result->program, stream))
					return &*slot.result;
		}
		return nullptr;
	}

	[[nodiscard]] const arm64::gadgets::BlockLink *link(
		std::uint64_t pc)
	{
		return &link_record(pc).link;
	}

	[[nodiscard]] arm64::gadgets::IndirectLink *indirect_link(
		std::uint64_t source_pc)
	{
		const auto [created, inserted] =
			indirect_links_.try_emplace(source_pc);
		if (inserted) {
			constexpr auto empty_pc = ~std::uint64_t{};
			const auto *empty_link = link(empty_pc);
			for (auto &entry : created->second.entries) {
				entry.guest_pc = empty_pc;
				entry.link = empty_link;
			}
		}
		return &created->second;
	}

	void learn_indirect(arm64::gadgets::IndirectLink &site,
			    std::uint64_t target_pc)
	{
		const auto *target = link(target_pc);
		for (auto &entry : site.entries) {
			if (entry.guest_pc != target_pc)
				continue;
			entry.link = target;
			return;
		}
		auto &entry = site.entries[site.next_replacement & 1U];
		entry.guest_pc = target_pc;
		entry.link = target;
		site.next_replacement ^= 1U;
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
		++slot.activation;
		if (slot.activation == 0)
			slot.activation = 1;
		activate(pc, slot);
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
	static constexpr unsigned stream_index_granule_shift = 6;
	static constexpr std::size_t stream_index_ways = 4;
	static constexpr std::size_t stream_index_set_count = 16 * 1024;

	struct Slot final {
		std::optional<BuildResult> result;
		std::uint64_t pc{};
		std::uint64_t epoch{};
		std::uint64_t activation{};
	};

	struct Slab final {
		std::array<Slot, slab_slots> slots;
	};

	struct Location final {
		std::size_t set;
		std::size_t slab;
		std::size_t offset;
	};

	struct StreamIndexEntry final {
		/* A reused cache slot changes activation, invalidating every old
		 * index entry without walking the former program's stream.
		 */
		const Slot *slot{};
		std::uint64_t activation{};
	};

	struct StreamIndexSet final {
		std::array<StreamIndexEntry, stream_index_ways> entries;
		std::uint8_t next_replacement{};
	};

	struct LinkRecord final {
		explicit LinkRecord(std::uint64_t pc) noexcept
			: fallback_stream{
				reinterpret_cast<std::uintptr_t>(
					uml_arm64_gadget_link_exit),
				static_cast<std::uintptr_t>(pc)},
			  fp_stream{
				reinterpret_cast<std::uintptr_t>(
					uml_arm64_gadget_fp_link_entry),
				0},
			  link{
				.stream = fallback_stream.data(),
				.instruction_count = 0,
				.guest_pc = pc,
			  }
		{
		}

		void reset() noexcept
		{
			link.stream = fallback_stream.data();
			link.instruction_count = 0;
			fp_stream[1] = 0;
		}

		std::array<std::uintptr_t, 2> fallback_stream;
		std::array<std::uintptr_t, 2> fp_stream;
		arm64::gadgets::BlockLink link;
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

	[[nodiscard]] static std::size_t stream_index_location(
		std::uintptr_t stream) noexcept
	{
		const auto chunk = stream >> stream_index_granule_shift;
		const auto mixed = chunk ^ (chunk >> 14) ^ (chunk >> 28);
		return static_cast<std::size_t>(mixed) &
			(stream_index_set_count - 1);
	}

	template <typename Operation>
	static void for_each_stream_index_set(
		const arm64::gadgets::Program &program, Operation operation)
	{
		const auto words = program.words();
		if (words.empty())
			return;
		const auto begin = reinterpret_cast<std::uintptr_t>(words.data());
		const auto last = begin +
			words.size() * sizeof(std::uintptr_t) - 1;
		auto chunk = begin >> stream_index_granule_shift;
		const auto last_chunk = last >> stream_index_granule_shift;
		for (;;) {
			operation(stream_index_location(
				chunk << stream_index_granule_shift));
			if (chunk == last_chunk)
				break;
			++chunk;
		}
	}

	void index_stream(const Slot &slot)
	{
		const auto &result = *slot.result;
		for_each_stream_index_set(result.program, [&](std::size_t location) {
			auto &set = stream_index_[location];
			std::size_t selected =
				set.next_replacement & (stream_index_ways - 1);
			for (std::size_t way = 0; way < stream_index_ways; ++way) {
				const auto &entry = set.entries[way];
				const bool current =
					entry.slot != nullptr &&
					entry.slot->epoch == epoch_ &&
					entry.slot->activation ==
						entry.activation &&
					entry.slot->result;
				if (!current || entry.slot == &slot) {
					selected = way;
					break;
				}
			}
			set.entries[selected] = {
				.slot = &slot,
				.activation = slot.activation,
			};
			set.next_replacement = static_cast<std::uint8_t>(
				(selected + 1) & (stream_index_ways - 1));
		});
	}

	[[nodiscard]] LinkRecord &link_record(std::uint64_t pc)
	{
		return links_.try_emplace(pc, pc).first->second;
	}

	void activate(std::uint64_t pc, const Slot &slot)
	{
		const auto &result = *slot.result;
		if (!result.complete || result.program.instruction_count() == 0)
			return;
		index_stream(slot);
		auto &record = link_record(pc);
		auto *target = &record.link;
		if (result.program.accesses_fp_state()) {
			record.fp_stream[1] = reinterpret_cast<std::uintptr_t>(
				result.program.words().data());
			target->stream = record.fp_stream.data();
		} else {
			target->stream = result.program.words().data();
		}
		target->instruction_count = result.program.instruction_count();
	}

	void deactivate(std::uint64_t pc,
			const arm64::gadgets::Program &program) noexcept
	{
		const auto found = links_.find(pc);
		if (found == links_.end())
			return;
		auto &record = found->second;
		const auto *words = program.words().data();
		const bool direct = record.link.stream == words;
		const bool fp = record.link.stream == record.fp_stream.data() &&
			record.fp_stream[1] ==
				reinterpret_cast<std::uintptr_t>(words);
		if (direct || fp)
			record.reset();
	}

	std::pmr::unsynchronized_pool_resource program_storage_;
	std::unordered_map<std::uint64_t, LinkRecord> links_;
	std::unordered_map<std::uint64_t,
		arm64::gadgets::IndirectLink> indirect_links_;
	std::unique_ptr<StreamIndexSet[]> stream_index_;
	std::array<std::unique_ptr<Slab>, slab_count> slabs_;
	std::array<std::uint8_t, set_count> next_replacement_{};
	std::uint64_t epoch_{1};
};

GadgetExecutor::GadgetExecutor(arm64::gadgets::BuildOptions build_options)
	: programs_{std::make_unique<ProgramCache>()},
	  build_options_{build_options}
{
}

GadgetExecutor::~GadgetExecutor()
{
	if (current_cpu_ != nullptr) {
		current_cpu_->gadget_indirect_miss = nullptr;
		arm64::restore_native_fp_environment(*current_cpu_);
	}
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
	if (current_cpu_ != nullptr) {
		/* A miss is a pointer into this executor's cache, not architectural
		 * state. Learning the target can allocate after the guest branch has
		 * retired; discard the marker even when that postamble throws. */
		current_cpu_->gadget_indirect_miss = nullptr;
		arm64::restore_native_fp_environment(*current_cpu_);
	}
	current_cpu_ = nullptr;
	if (execution_context_ != nullptr)
		(void)darwin_uml_static_vm_execution_context_end(execution_context_);
}

void GadgetExecutor::select_cpu(CpuState &cpu) noexcept
{
	if (current_cpu_ == &cpu)
		return;
	if (current_cpu_ != nullptr) {
		current_cpu_->gadget_indirect_miss = nullptr;
		arm64::restore_native_fp_environment(*current_cpu_);
	}
	/* The marker is executor-private scratch. Never inherit it across CPU or
	 * execution reuse; end_execution() resets current_cpu_ at every exit. */
	cpu.gadget_indirect_miss = nullptr;
	current_cpu_ = &cpu;
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

const BuildResult &GadgetExecutor::store(
	std::uint64_t pc, BuildResult result)
{
	return programs_->store(pc, std::move(result));
}

::uml::cpu::EngineEvent GadgetExecutor::run_block(
	arm64::CpuState &cpu, StaticGuestMemory &memory, unsigned long generation,
	std::size_t &executed)
{
	executed = 0;
	floating_point_syndrome_ = 0;
	select_cpu(cpu);
	auto *context = execution_context(generation);
	if (context == nullptr)
		return ::uml::cpu::EngineEvent::fatal;
	std::array<std::uint32_t, block_instruction_count> instructions;
	const auto sequences =
		darwin_uml_static_vm_execution_context_sequences(context);
	preempted_ = sequences.execution_interrupt != 0;
	if (preempted_)
		return ::uml::cpu::EngineEvent::interrupt;
	if (mapping_generation_ != generation ||
	    mapping_changes_ != sequences.mapping_changes) {
		memory.prepare(generation, sequences.mapping_changes);
		tlb_.prepare(generation, sequences.mapping_changes);
		mapping_generation_ = generation;
		mapping_changes_ = sequences.mapping_changes;
	}
	const auto guest_fpcr = cpu.fp_loaded ? cpu.fpcr : cpu.deferred_fpcr;
	if ((guest_fpcr &
	     arm64::detail::fpcr_exception_enable_mask) != 0U)
		return run_trap_enabled_instruction(cpu, memory, executed);
	prepare_cache(generation, sequences.code_changes);
	auto selected = find(cpu.pc);
	if (!selected) {
		const auto count = fetch_instruction_window(
			memory, cpu.pc, instructions);
		if (count == 0) {
			const auto fault = memory.last_fault();
			tlb_.fault_address = fault.address;
			tlb_.fault_access = ::uml::memory::translated_access(
				fault.access);
			return ::uml::cpu::EngineEvent::memory_fault;
		}
		const auto window = std::span{instructions}.first(count);
		auto decoded = arm64::gadgets::Builder{}.build(
			cpu.pc, window,
			[this](std::uint64_t pc) {
				return programs_->link(pc);
			},
			programs_->program_storage(),
			[this](std::uint64_t source_pc) {
				return programs_->indirect_link(source_pc);
			},
			build_options_);
		selected = std::cref(store(cpu.pc, std::move(decoded)));
	}
	const auto &built = selected->get();
	if (built.program.instruction_count() == 0) {
		cpu.pc = built.stop_address;
		return ::uml::cpu::EngineEvent::illegal_instruction;
	}
	const auto event =
		arm64::gadgets::execute_preserving_native_fp_environment(
			built.program, cpu, tlb_, executed);
	const bool fpcr_enable_exit =
		event == ::uml::cpu::EngineEvent::continued &&
		(effective_fpcr(cpu) &
		 arm64::detail::fpcr_exception_enable_mask) != 0U;
	const bool count_exit =
		terminal_does_not_retire(event) || fpcr_enable_exit;
	if (count_exit) {
		const auto *current_program = &built.program;
		const auto *exit_stream = cpu.gadget_indirect_miss;
		if (!program_contains_stream(*current_program, exit_stream)) {
			if (const auto *cached =
				    programs_->find_stream(exit_stream);
			    cached != nullptr)
				current_program = &cached->program;
		}
		if (fpcr_enable_exit)
			correct_fpcr_enable_instruction_count(
				*current_program, event, cpu, executed);
		else
			correct_terminal_instruction_count(
				*current_program, event, cpu.pc, executed);
		cpu.gadget_indirect_miss = nullptr;
	}
	if (cpu.gadget_indirect_miss != nullptr &&
	    (cpu.pc & (alignof(std::uint32_t) - 1U)) == 0U) {
		programs_->learn_indirect(
			*static_cast<arm64::gadgets::IndirectLink *>(
				cpu.gadget_indirect_miss),
			cpu.pc);
	}
	cpu.gadget_indirect_miss = nullptr;
	return event;
}

::uml::cpu::EngineEvent GadgetExecutor::run_trap_enabled_instruction(
	arm64::CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed)
{
	/* Guest trap enables are intentionally a cold execution mode.  Limiting
	 * every translated run to one instruction guarantees that an integer or
	 * memory instruction cannot carry execution past a later FP/SIMD
	 * data-processing instruction in the same cached block.
	 */
	cpu.gadget_indirect_miss = nullptr;
	arm64::gadgets::restore_native_fp_environment(cpu);
	std::uint32_t instruction = 0;
	auto encoded = std::as_writable_bytes(std::span{&instruction, 1});
	if (!memory.fetch(cpu.pc, encoded)) {
		const auto fault = memory.last_fault();
		tlb_.fault_address = fault.address;
		tlb_.fault_access = ::uml::memory::translated_access(fault.access);
		return ::uml::cpu::EngineEvent::memory_fault;
	}

	auto built = arm64::gadgets::Builder{}.build(
		cpu.pc, std::span{&instruction, 1}, {},
		std::pmr::get_default_resource(), {}, build_options_);
	if (!built.complete || built.program.instruction_count() != 1) {
		cpu.pc = built.stop_address;
		return ::uml::cpu::EngineEvent::illegal_instruction;
	}

	const bool transactional =
		arm64::detail::needs_floating_exception_transaction(
			cpu.fp_loaded ? cpu.fpcr : cpu.deferred_fpcr,
			instruction);
	if (!transactional) {
		const auto event = arm64::gadgets::execute(
			built.program, cpu, tlb_, executed);
		correct_terminal_instruction_count(
			built.program, event, cpu.pc, executed);
		cpu.gadget_indirect_miss = nullptr;
		/* A one-instruction fallthrough ends in Builder's synthetic
		 * interrupt gadget.  It is an internal cold-mode boundary, not a
		 * guest-visible fairness interrupt.
		 */
		return event == ::uml::cpu::EngineEvent::interrupt ?
			::uml::cpu::EngineEvent::continued : event;
	}

	cpu.access_fp_state();
	const auto original_fpsr = cpu.fpsr;
	arm64::CpuState isolated = cpu;
	isolated.fpsr &=
		~arm64::detail::fpsr_exception_cumulative_mask;

	std::size_t isolated_executed = 0;
	const auto event = arm64::gadgets::execute(
		built.program, isolated, tlb_, isolated_executed);
	if (event != ::uml::cpu::EngineEvent::interrupt &&
	    event != ::uml::cpu::EngineEvent::continued) {
		/* Reserved allocations and other non-executing events retain the
		 * live architectural state, just like the strict interpreter.
		 */
		correct_terminal_instruction_count(
			built.program, event, isolated.pc, isolated_executed);
		isolated.gadget_indirect_miss = nullptr;
		executed = isolated_executed;
		return event;
	}

	const auto decision = arm64::detail::decide_floating_exception(
		cpu.fpcr, original_fpsr, isolated.fpsr);
	if (decision.traps()) {
		cpu.fpsr = decision.committed_fpsr;
		floating_point_syndrome_ = decision.syndrome;
		executed = 0;
		return ::uml::cpu::EngineEvent::floating_point_trap;
	}

	/* The scratch execution began with cumulative flags clear.  Preserve
	 * every other FPSR field produced by the gadget while restoring the
	 * pre-existing cumulative state before committing the transaction.
	 */
	isolated.fpsr =
		(isolated.fpsr &
		 ~arm64::detail::fpsr_exception_cumulative_mask) |
		(decision.committed_fpsr &
		 arm64::detail::fpsr_exception_cumulative_mask);
	cpu = isolated;
	executed = isolated_executed;
	return ::uml::cpu::EngineEvent::continued;
}

std::uint64_t GadgetExecutor::fault_address() const noexcept
{
	return tlb_.fault_address;
}

::uml::memory::Access GadgetExecutor::fault_access() const noexcept
{
	return tlb_.fault_access;
}

std::uint32_t GadgetExecutor::floating_point_syndrome() const noexcept
{
	return floating_point_syndrome_;
}

} // namespace uml::cpu::arm64::gadgets
