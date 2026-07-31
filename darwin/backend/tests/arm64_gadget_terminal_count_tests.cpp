// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"

#include "cpu/arm64/detail/floating_exception.hpp"
#include "runtime/static/gadget_executor.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <sys/mman.h>

namespace arm64 = uml::backend::arm64;
namespace gadgets = arm64::gadgets;
using uml::backend::GadgetExecutor;
using uml::backend::StaticGuestMemory;
using namespace uml::backend::tests;

namespace {

constexpr std::uint64_t code_base = 0x82'0000;

struct Outcome final {
	gadgets::Event event{gadgets::Event::fatal};
	arm64::CpuState cpu{};
	std::size_t executed{};
	std::uint64_t fault_address{};
	gadgets::Access fault_access{gadgets::Access::read};
};

using Configure = void (*)(arm64::CpuState &);

[[nodiscard]] Outcome execute_block(GadgetExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation,
	arm64::CpuState cpu)
{
	require(executor.begin_execution(generation, cpu.pc));
	std::size_t executed = 0;
	const auto event =
		executor.run_block(cpu, memory, generation, executed);
	executor.end_execution();
	gadgets::restore_native_fp_environment(cpu);
	return {
		.event = event,
		.cpu = cpu,
		.executed = executed,
		.fault_address = executor.fault_address(),
		.fault_access = executor.fault_access(),
	};
}

[[nodiscard]] Outcome run(std::span<const std::uint32_t> instructions,
	Configure configure = nullptr, gadgets::BuildOptions options = {})
{
	static unsigned long next_generation = 740;
	const auto generation = next_generation++;

	JitBacking backing{1};
	for (std::size_t index = 0; index < instructions.size(); ++index)
		backing.put32(index * sizeof(std::uint32_t), instructions[index]);
	JitMapping mapping{
		generation, code_base, backing, 1, PROT_READ | PROT_EXEC};
	StaticGuestMemory memory;
	GadgetExecutor executor{options};
	arm64::CpuState cpu{};
	cpu.pc = code_base;
	if (configure != nullptr)
		configure(cpu);
	return execute_block(executor, memory, generation, cpu);
}

[[nodiscard]] std::uint32_t direct_branch(
	std::uint64_t source, std::uint64_t target)
{
	const auto displacement =
		static_cast<std::int64_t>(target) -
		static_cast<std::int64_t>(source);
	require((displacement & 3) == 0);
	return 0x1400'0000U |
		(static_cast<std::uint32_t>(displacement >> 2) & 0x03ff'ffffU);
}

[[nodiscard]] constexpr std::size_t program_cache_set(
	std::uint64_t pc) noexcept
{
	const auto block = pc >> 2;
	const auto mixed = block ^ (block >> 12) ^ (block >> 24);
	return static_cast<std::size_t>(mixed) & (4 * 1024 - 1);
}

void illegal_at_block_start_does_not_retire()
{
	constexpr std::array instructions{
		0x0000'0000U,
		0xd400'0001U, // svc #0
	};
	const auto outcome = run(instructions);
	require(outcome.event == gadgets::Event::illegal_instruction);
	require(outcome.cpu.pc == code_base);
	require(outcome.executed == 0);
}

void valid_prefix_before_illegal_is_counted()
{
	constexpr std::array instructions{
		0xd280'00a0U, // movz x0, #5
		0x0000'0000U,
		0xd400'0001U, // svc #0
	};
	const auto outcome = run(instructions);
	require(outcome.event == gadgets::Event::illegal_instruction);
	require(outcome.cpu.registers[0] == 5);
	require(outcome.cpu.pc == code_base + sizeof(std::uint32_t));
	require(outcome.executed == 1);
}

void memory_fault_excludes_fault_and_later_decoded_words()
{
	constexpr std::array instructions{
		0xd280'00a0U, // movz x0, #5
		0xf940'0041U, // ldr x1, [x2]
		0xd280'00e3U, // movz x3, #7
		0xd400'0001U, // svc #0
	};
	const auto outcome = run(instructions, [](arm64::CpuState &cpu) {
		cpu.registers[2] = code_base + DARWIN_UML_GUEST_PAGE_SIZE;
	});
	require(outcome.event == gadgets::Event::memory_fault);
	require(outcome.cpu.registers[0] == 5);
	require(outcome.cpu.registers[1] == 0);
	require(outcome.cpu.registers[3] == 0);
	require(outcome.cpu.pc == code_base + sizeof(std::uint32_t));
	require(outcome.executed == 1);
	require(outcome.fault_address ==
		code_base + DARWIN_UML_GUEST_PAGE_SIZE);
	require(outcome.fault_access == gadgets::Access::read);
}

void enabled_trap_reserved_allocation_does_not_retire()
{
	constexpr std::array instructions{
		0x2e00'fc00U,
		0xd400'0001U, // svc #0
	};
	const auto outcome = run(instructions, [](arm64::CpuState &cpu) {
		cpu.fpcr = arm64::detail::fpcr_exception_enable_mask;
	});
	require(outcome.event == gadgets::Event::illegal_instruction);
	require(outcome.cpu.pc == code_base);
	require(outcome.executed == 0);
}

void valid_prefix_before_fatal_is_counted()
{
	constexpr std::array instructions{
		0xd280'00a0U, // movz x0, #5
		0xd500'0000U, // unsupported system allocation
		0xd400'0001U, // svc #0
	};
	const auto outcome = run(instructions);
	require(outcome.event == gadgets::Event::fatal);
	require(outcome.cpu.registers[0] == 5);
	require(outcome.cpu.pc == code_base + sizeof(std::uint32_t));
	require(outcome.executed == 1);
	require(outcome.cpu.gadget_indirect_miss == nullptr);
}

void gcs_fault_excludes_the_faulting_instruction()
{
	constexpr std::array instructions{
		0xd280'00a0U, // movz x0, #5
		0xd91f'0fffU, // gcsstr xzr, [sp]
		0xd400'0001U, // svc #0
	};
	gadgets::BuildOptions options{};
	options.enable_guarded_control_flow = true;
	const auto outcome = run(
		instructions,
		[](arm64::CpuState &cpu) {
			cpu.gcs_implemented = true;
			cpu.gcs_store_enabled = false;
			cpu.sp = code_base;
		},
		options);
	require(outcome.event == gadgets::Event::gcs_fault);
	require(outcome.cpu.registers[0] == 5);
	require(outcome.cpu.pc == code_base + sizeof(std::uint32_t));
	require(outcome.executed == 1);
}

void retiring_terminals_keep_their_instruction_count()
{
	{
		constexpr std::array instructions{
			0xd400'0001U, // svc #0
		};
		const auto outcome = run(instructions);
		require(outcome.event == gadgets::Event::system_call);
		require(outcome.cpu.pc == code_base + sizeof(std::uint32_t));
		require(outcome.executed == 1);
	}
	{
		constexpr std::array instructions{
			0xd420'0000U, // brk #0
		};
		const auto outcome = run(instructions);
		require(outcome.event == gadgets::Event::breakpoint);
		require(outcome.cpu.pc == code_base);
		require(outcome.executed == 1);
	}
}

void linked_successor_terminal_subtracts_only_its_suffix()
{
	constexpr unsigned long generation = 760;
	constexpr auto target = code_base + 0x100;
	JitBacking backing{1};
	backing.put32(0, direct_branch(code_base, target));
	backing.put32(0x100, 0xd280'0121U); // movz x1, #9
	backing.put32(0x104, 0x0000'0000U);
	backing.put32(0x108, 0xd400'0001U); // svc #0
	JitMapping mapping{
		generation, code_base, backing, 1, PROT_READ | PROT_EXEC};
	StaticGuestMemory memory;
	GadgetExecutor executor;

	arm64::CpuState warm_cpu{};
	warm_cpu.pc = target;
	const auto warm = execute_block(
		executor, memory, generation, warm_cpu);
	require(warm.event == gadgets::Event::illegal_instruction);
	require(warm.executed == 1);
	require(warm.cpu.gadget_indirect_miss == nullptr);

	arm64::CpuState linked_cpu{};
	linked_cpu.pc = code_base;
	const auto linked = execute_block(
		executor, memory, generation, linked_cpu);
	require(linked.event == gadgets::Event::illegal_instruction);
	require(linked.cpu.pc == target + sizeof(std::uint32_t));
	require(linked.cpu.registers[1] == 9);
	require(linked.executed == 2);
	require(linked.cpu.gadget_indirect_miss == nullptr);
}

void linked_successor_fpcr_exit_subtracts_only_its_suffix()
{
	constexpr unsigned long generation = 761;
	constexpr auto target = code_base + 0x100;
	JitBacking backing{1};
	backing.put32(0, direct_branch(code_base, target));
	backing.put32(0x100, 0xd51b'4409U); // msr fpcr, x9
	backing.put32(0x104, 0xd280'00e0U); // movz x0, #7
	backing.put32(0x108, 0xd400'0001U); // svc #0
	JitMapping mapping{
		generation, code_base, backing, 1, PROT_READ | PROT_EXEC};
	StaticGuestMemory memory;
	GadgetExecutor executor;

	arm64::CpuState warm_cpu{};
	warm_cpu.pc = target;
	const auto warm = execute_block(
		executor, memory, generation, warm_cpu);
	require(warm.event == gadgets::Event::system_call);
	require(warm.executed == 3);
	require(warm.cpu.gadget_indirect_miss == nullptr);

	arm64::CpuState linked_cpu{};
	linked_cpu.pc = code_base;
	linked_cpu.registers[9] =
		arm64::detail::fpcr_exception_enable_mask;
	const auto linked = execute_block(
		executor, memory, generation, linked_cpu);
	require(linked.event == gadgets::Event::continued);
	require(linked.cpu.pc == target + sizeof(std::uint32_t));
	require(linked.cpu.fpcr ==
		arm64::detail::fpcr_exception_enable_mask);
	require(linked.cpu.registers[0] == 0);
	require(linked.executed == 2);
	require(linked.cpu.gadget_indirect_miss == nullptr);
}

void reused_cache_slots_leave_no_live_stream_index_entries()
{
	constexpr unsigned long generation = 763;
	constexpr auto source = code_base;
	constexpr auto target = code_base + 0x100;
	constexpr std::size_t cache_ways = 4;
	std::array<std::uint64_t, cache_ways + 1> colliding{};
	colliding[0] = target;
	std::size_t found = 1;
	for (auto candidate = target + sizeof(std::uint32_t);
	     found < colliding.size(); candidate += sizeof(std::uint32_t)) {
		require(candidate - code_base < 64 * 1024 * 1024);
		if (program_cache_set(candidate) == program_cache_set(target))
			colliding[found++] = candidate;
	}
	require(program_cache_set(source) != program_cache_set(target));

	const auto mapped_bytes =
		colliding.back() - code_base + sizeof(std::uint32_t);
	const auto pages = static_cast<std::size_t>(
		(mapped_bytes + DARWIN_UML_GUEST_PAGE_SIZE - 1) /
		DARWIN_UML_GUEST_PAGE_SIZE);
	JitBacking backing{pages};
	backing.put32(source - code_base, direct_branch(source, target));
	backing.put32(target - code_base, 0xd280'0121U); // movz x1, #9
	backing.put32(target - code_base + 4, 0x0000'0000U);
	backing.put32(target - code_base + 8, 0xd400'0001U); // svc #0
	for (std::size_t index = 1; index < colliding.size(); ++index)
		backing.put32(
			colliding[index] - code_base, 0xd400'0001U);
	JitMapping mapping{
		generation, code_base, backing, pages, PROT_READ | PROT_EXEC};
	StaticGuestMemory memory;
	GadgetExecutor executor;

	arm64::CpuState warm_target{};
	warm_target.pc = target;
	require(execute_block(
			executor, memory, generation, warm_target).executed == 1);

	arm64::CpuState warm_source{};
	warm_source.pc = source;
	const auto first = execute_block(
		executor, memory, generation, warm_source);
	require(first.event == gadgets::Event::illegal_instruction);
	require(first.cpu.registers[1] == 9);
	require(first.executed == 2);

	/* Four more translations replace every way in target's cache set. */
	for (std::size_t index = 1; index < colliding.size(); ++index) {
		arm64::CpuState collision{};
		collision.pc = colliding[index];
		require(execute_block(
				executor, memory, generation, collision).event ==
			gadgets::Event::system_call);
	}

	arm64::CpuState rewarm_target{};
	rewarm_target.pc = target;
	require(execute_block(
			executor, memory, generation, rewarm_target).executed == 1);

	arm64::CpuState linked{};
	linked.pc = source;
	const auto second =
		execute_block(executor, memory, generation, linked);
	require(second.event == gadgets::Event::illegal_instruction);
	require(second.cpu.pc == target + sizeof(std::uint32_t));
	require(second.cpu.registers[1] == 9);
	require(second.executed == 2);
	require(second.cpu.gadget_indirect_miss == nullptr);
}

void indirect_link_learning_still_uses_and_clears_scratch()
{
	constexpr unsigned long generation = 762;
	constexpr auto target = code_base + 0x100;
	JitBacking backing{1};
	backing.put32(0, 0xd61f'00a0U); // br x5
	backing.put32(0x100, 0xd280'0160U); // movz x0, #11
	backing.put32(0x104, 0xd400'0001U); // svc #0
	JitMapping mapping{
		generation, code_base, backing, 1, PROT_READ | PROT_EXEC};
	StaticGuestMemory memory;
	GadgetExecutor executor;

	arm64::CpuState warm_cpu{};
	warm_cpu.pc = target;
	const auto warm = execute_block(
		executor, memory, generation, warm_cpu);
	require(warm.event == gadgets::Event::system_call);
	require(warm.executed == 2);

	arm64::CpuState miss_cpu{};
	miss_cpu.pc = code_base;
	miss_cpu.registers[5] = target;
	const auto miss = execute_block(
		executor, memory, generation, miss_cpu);
	require(miss.event == gadgets::Event::continued);
	require(miss.cpu.pc == target);
	require(miss.executed == 1);
	require(miss.cpu.gadget_indirect_miss == nullptr);

	arm64::CpuState hit_cpu{};
	hit_cpu.pc = code_base;
	hit_cpu.registers[5] = target;
	const auto hit = execute_block(
		executor, memory, generation, hit_cpu);
	require(hit.event == gadgets::Event::system_call);
	require(hit.cpu.pc == target + 2 * sizeof(std::uint32_t));
	require(hit.cpu.registers[0] == 11);
	require(hit.executed == 3);
	require(hit.cpu.gadget_indirect_miss == nullptr);
}

} // namespace

int main()
{
	illegal_at_block_start_does_not_retire();
	valid_prefix_before_illegal_is_counted();
	memory_fault_excludes_fault_and_later_decoded_words();
	enabled_trap_reserved_allocation_does_not_retire();
	valid_prefix_before_fatal_is_counted();
	gcs_fault_excludes_the_faulting_instruction();
	retiring_terminals_keep_their_instruction_count();
	linked_successor_terminal_subtracts_only_its_suffix();
	linked_successor_fpcr_exit_subtracts_only_its_suffix();
	reused_cache_slots_leave_no_live_stream_index_entries();
	indirect_link_learning_still_uses_and_clears_scratch();
	return 0;
}
