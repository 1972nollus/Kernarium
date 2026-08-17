// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"

#include "cpu/arm64/gadgets/executor.hpp"

#include <cstdint>
#include <cstring>
#include <memory>

#ifndef UML_GADGET_INLINE_CALLS
#define UML_GADGET_INLINE_CALLS 1
#endif

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
using uml::emulator::tests::JitBacking;
using uml::emulator::tests::JitMapping;
using uml::emulator::tests::require;
using uml::memory::StaticGuestMemory;

namespace {

constexpr std::uint64_t code_base = 0xd0'0000;
constexpr std::uint32_t system_call = 0xd400'0001U;

using Executor = std::unique_ptr<gadgets::GadgetExecutor>;
using Memory = std::unique_ptr<StaticGuestMemory>;

struct Outcome final {
	::uml::cpu::EngineEvent event;
	std::size_t executed;
};

[[nodiscard]] Outcome run_once(gadgets::GadgetExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation, arm64::CpuState &cpu)
{
	require(executor.begin_execution(generation, cpu.pc));
	std::size_t executed = 0;
	const auto event = executor.run_block(cpu, memory, generation, executed);
	executor.end_execution();
	return {event, executed};
}

[[nodiscard]] constexpr std::uint32_t move_immediate(
	unsigned reg, unsigned value) noexcept
{
	return 0xd280'0000U | ((value & 0xffffU) << 5U) | (reg & 31U);
}

/* CBZ Xn, +offset */
[[nodiscard]] constexpr std::uint32_t compare_branch_zero(
	unsigned reg, int words) noexcept
{
	return 0xb400'0000U |
		((static_cast<std::uint32_t>(words) & 0x7ffffU) << 5U) |
		(reg & 31U);
}

/* A block whose conditional branch is followed by more translated
 * instructions. Taking the branch leaves from the middle of the block, which
 * before the fall-through was inlined could not happen: every exit was a
 * block's last instruction. */
void write_block(JitBacking &backing, unsigned taken)
{
	/* 0: mov x1, #1        -- retires
	 * 4: cbz x0, +5 words  -- retires, and leaves if x0 == 0
	 * 8: mov x2, #2        -- only on the fall-through
	 * c: mov x3, #3
	 * 10: mov x4, #4
	 * 14: svc
	 * 18: (branch target) mov x5, #5
	 * 1c: svc
	 */
	backing.put32(0x00, move_immediate(1, 1));
	backing.put32(0x04, compare_branch_zero(0, taken));
	backing.put32(0x08, move_immediate(2, 2));
	backing.put32(0x0c, move_immediate(3, 3));
	backing.put32(0x10, move_immediate(4, 4));
	backing.put32(0x14, system_call);
	backing.put32(0x18, move_immediate(5, 5));
	backing.put32(0x1c, system_call);
}

/* The fall-through path retires every instruction the block translated. */
void the_fall_through_retires_the_whole_block()
{
	constexpr unsigned long generation = 940;
	JitBacking backing{1};
	write_block(backing, 5);
	JitMapping mapping{generation, code_base, backing, 1};

	Memory memory{std::make_unique<StaticGuestMemory>()};
	auto executor = std::make_unique<gadgets::GadgetExecutor>();
	arm64::CpuState cpu{};
	cpu.pc = code_base;
	cpu.registers[0] = 1; /* nonzero: the branch is not taken */
	const auto outcome = run_once(*executor, *memory, generation, cpu);
	require(outcome.event == ::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[1] == 1);
	require(cpu.registers[2] == 2);
	require(cpu.registers[3] == 3);
	require(cpu.registers[4] == 4);
	/* mov, cbz, mov, mov, mov, svc */
	require(outcome.executed == 6);
}

/* Taking the branch retires only what ran before it.
 *
 * This is the count the change had to keep honest. Entering a block precharges
 * its whole translated length, and with the fall-through inlined the branch is
 * no longer the last instruction, so the taken path has to give the rest back.
 * Getting this wrong is invisible to every conformance test -- the guest's
 * registers are identical either way -- and shows up only as a wrong retirement
 * count.
 */
void taking_the_branch_retires_only_what_ran()
{
	constexpr unsigned long generation = 941;
	JitBacking backing{1};
	write_block(backing, 5);
	JitMapping mapping{generation, code_base, backing, 1};

	Memory memory{std::make_unique<StaticGuestMemory>()};
	auto executor = std::make_unique<gadgets::GadgetExecutor>();
	arm64::CpuState cpu{};
	cpu.pc = code_base;
	cpu.registers[0] = 0; /* zero: the branch is taken */
	const auto outcome = run_once(*executor, *memory, generation, cpu);
	/* The target has not been translated yet, so its link is still the
	 * unlinked exit and the branch leaves here. */
	require(outcome.event == ::uml::cpu::EngineEvent::continued);
	require(cpu.pc == code_base + 0x18);
	require(cpu.registers[1] == 1);
	/* The instructions translated behind the branch must not have run. */
	require(cpu.registers[2] == 0);
	require(cpu.registers[3] == 0);
	require(cpu.registers[4] == 0);
	/* The block precharged all six instructions it translated. Two ran, so
	 * the taken path had to give four back. */
	require(outcome.executed == 2);

	const auto target = run_once(*executor, *memory, generation, cpu);
	require(target.event == ::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[5] == 5);
	require(target.executed == 2);
}

/* A fault after the branch still lands on the right instruction.
 *
 * Fault correction converts a faulting PC to its position with
 * (pc - base) / 4, which stays exact only because a fall-through is the next
 * address. This runs into a translated instruction past the branch and
 * requires the retirement count to match the address it stopped at.
 */
void a_fault_past_the_branch_counts_from_its_address()
{
	constexpr unsigned long generation = 942;
	JitBacking backing{1};
	write_block(backing, 5);
	/* Replace the third instruction after the branch with an undefined
	 * encoding, so the block translates but stops there when executed. */
	backing.put32(0x10, 0x0000'0000U);
	JitMapping mapping{generation, code_base, backing, 1};

	Memory memory{std::make_unique<StaticGuestMemory>()};
	auto executor = std::make_unique<gadgets::GadgetExecutor>();
	arm64::CpuState cpu{};
	cpu.pc = code_base;
	cpu.registers[0] = 1; /* not taken: fall through into the fault */
	const auto outcome = run_once(*executor, *memory, generation, cpu);
	require(outcome.event ==
		::uml::cpu::EngineEvent::illegal_instruction);
	require(cpu.pc == code_base + 0x10);
	/* mov, cbz, mov, mov retired; the undefined word did not. */
	require(outcome.executed == 4);
}

#if UML_GADGET_INLINE_CALLS
/* A direct call whose callee is translated inline behind it.
 *
 * The block becomes three runs -- caller, callee, and the continuation after
 * the return -- so this is also what proves the run list works: the guest
 * addresses are no longer contiguous, and both the retirement count and the
 * stale-program check have to cope.
 */
void a_call_translates_its_callee_inline()
{
	constexpr unsigned long generation = 944;
	JitBacking backing{1};
	/* 0x00: mov x1, #1
	 * 0x04: bl +8 words      -- to 0x24
	 * 0x08: mov x2, #2       -- the return continuation
	 * 0x0c: svc
	 * 0x24: mov x3, #3       -- the callee
	 * 0x28: ret
	 */
	backing.put32(0x00, move_immediate(1, 1));
	backing.put32(0x04, 0x9400'0000U | 8U);
	backing.put32(0x08, move_immediate(2, 2));
	backing.put32(0x0c, system_call);
	backing.put32(0x24, move_immediate(3, 3));
	backing.put32(0x28, 0xd65f'03c0U);
	JitMapping mapping{generation, code_base, backing, 1};

	Memory memory{std::make_unique<StaticGuestMemory>()};
	auto executor = std::make_unique<gadgets::GadgetExecutor>();
	arm64::CpuState cpu{};
	cpu.pc = code_base;
	const auto outcome = run_once(*executor, *memory, generation, cpu);
	require(outcome.event == ::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[1] == 1);
	require(cpu.registers[3] == 3);
	/* Reached only by returning through the guard into the continuation. */
	require(cpu.registers[2] == 2);
	require(cpu.registers[30] == code_base + 0x08);
	/* mov, bl, mov, ret, mov, svc -- every instruction of all three runs. */
	require(outcome.executed == 6);
}
#endif

} // namespace

int main()
{
	the_fall_through_retires_the_whole_block();
	taking_the_branch_retires_only_what_ran();
	a_fault_past_the_branch_counts_from_its_address();
#if UML_GADGET_INLINE_CALLS
	a_call_translates_its_callee_inline();
#endif
	return 0;
}
