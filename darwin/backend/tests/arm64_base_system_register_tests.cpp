// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>

namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

constexpr std::uint64_t pc = 0x4a0'0000;
constexpr std::uint32_t fpcr_mask = 0x07f7'9f07U;
constexpr std::uint32_t fpsr_mask = 0xf800'009fU;

class NullMemory final : public arm64::GuestMemory {
public:
	[[nodiscard]] bool read(std::uint64_t, std::span<std::byte>) override
	{
		++reads;
		return false;
	}
	[[nodiscard]] bool write(
		std::uint64_t, std::span<const std::byte>) override
	{
		++writes;
		return false;
	}
	std::size_t reads{};
	std::size_t writes{};
};

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition) {
		std::fprintf(stderr, "require failed at %s:%u\n",
			location.file_name(), location.line());
		std::abort();
	}
}

[[nodiscard]] arm64::CpuState initial_state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] = 0x8102'0304'0506'0708ULL ^
			(static_cast<std::uint64_t>(index) * 0x0101'0101'0101'0101ULL);
	cpu.sp = 0xfeed'face'cafe'bee0ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0xffff'ffffU;
	cpu.fpsr = 0xffff'ffffU;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	cpu.exclusive_address = 0x2000;
	cpu.exclusive_key = 0x3456;
	cpu.exclusive_size = 8;
	return cpu;
}

struct Results final {
	arm64::CpuState interpreter;
	arm64::CpuState gadget;
};

[[nodiscard]] Results run(std::uint32_t instruction,
	const arm64::CpuState &initial)
{
	NullMemory memory;
	auto interpreted = initial;
	interpreted.pc = pc;
	require(arm64::Dispatcher{}.execute(interpreted, memory, instruction).reason ==
		arm64::StopReason::continued);
	require(memory.reads == 0 && memory.writes == 0);

	const std::array stream{instruction, 0xd400'0001U};
	const auto built = gadgets::Builder{}.build(pc, stream);
	require(built.complete);
	auto gadget = initial;
	gadget.pc = pc;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, gadget, *tlb) ==
		gadgets::Event::system_call);
	require(interpreted.pc == pc + 4 && gadget.pc == pc + 8);
	return {interpreted, gadget};
}

void require_illegal(std::uint32_t instruction)
{
	NullMemory memory;
	const auto initial = initial_state();
	auto interpreted = initial;
	interpreted.pc = pc;
	const auto result = arm64::Dispatcher{}.execute(
		interpreted, memory, instruction);
	require(result.reason == arm64::StopReason::illegal_instruction);
	require(result.address == pc && result.instruction == instruction);
	require(interpreted.pc == pc && interpreted.registers == initial.registers);
	require(interpreted.sp == initial.sp && interpreted.tls == initial.tls);
	require(interpreted.fpcr == initial.fpcr &&
		interpreted.fpsr == initial.fpsr && interpreted.nzcv == initial.nzcv);
	require(memory.reads == 0 && memory.writes == 0);

	const std::array stream{instruction, 0xd400'0001U};
	const auto built = gadgets::Builder{}.build(pc, stream);
	require(built.complete);
	auto gadget = initial;
	gadget.pc = pc;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, gadget, *tlb) ==
		gadgets::Event::illegal_instruction);
	require(gadget.pc == pc && gadget.registers == initial.registers);
	require(gadget.sp == initial.sp && gadget.tls == initial.tls);
	require(gadget.fpcr == initial.fpcr && gadget.fpsr == initial.fpsr &&
		gadget.nzcv == initial.nzcv);
}

void thread_pointer()
{
	for (unsigned transfer = 0; transfer < 32; ++transfer) {
		auto initial = initial_state();
		const auto read = run(0xd53b'd040U | transfer, initial);
		for (const auto *cpu : {&read.interpreter, &read.gadget}) {
			if (transfer != 31)
				require(cpu->registers[transfer] == initial.tls);
			else
				require(cpu->registers == initial.registers);
			require(cpu->tls == initial.tls);
		}

		initial = initial_state();
		const auto expected = transfer == 31 ? 0 : initial.registers[transfer];
		const auto write = run(0xd51b'd040U | transfer, initial);
		require(write.interpreter.tls == expected);
		require(write.gadget.tls == expected);
	}
}

void floating_control_status()
{
	for (unsigned transfer = 0; transfer < 32; ++transfer) {
		for (const auto [read_opcode, write_opcode, mask] :
			{std::array<std::uint32_t, 3>{
				0xd53b'4400U, 0xd51b'4400U, fpcr_mask},
			 std::array<std::uint32_t, 3>{
				0xd53b'4420U, 0xd51b'4420U, fpsr_mask}}) {
			auto initial = initial_state();
			const auto read = run(read_opcode | transfer, initial);
			if (transfer != 31) {
				require(read.interpreter.registers[transfer] == mask);
				require(read.gadget.registers[transfer] == mask);
			} else {
				require(read.interpreter.registers == initial.registers);
				require(read.gadget.registers == initial.registers);
			}

			initial = initial_state();
			const auto source = transfer == 31 ? 0U :
				static_cast<std::uint32_t>(initial.registers[transfer]);
			const auto write = run(write_opcode | transfer, initial);
			if (write_opcode == 0xd51b'4400U) {
				require(write.interpreter.fpcr == (source & mask));
				require(write.gadget.fpcr == (source & mask));
			} else {
				require(write.interpreter.fpsr == (source & mask));
				require(write.gadget.fpsr == (source & mask));
			}
		}
	}
}

void condition_flags()
{
	for (unsigned transfer = 0; transfer < 32; ++transfer) {
		auto initial = initial_state();
		const auto read = run(0xd53b'4200U | transfer, initial);
		if (transfer != 31) {
			require(read.interpreter.registers[transfer] == initial.nzcv);
			require(read.gadget.registers[transfer] == initial.nzcv);
		}

		initial = initial_state();
		const auto source = transfer == 31 ? 0U :
			static_cast<std::uint32_t>(initial.registers[transfer]);
		const auto write = run(0xd51b'4200U | transfer, initial);
		require(write.interpreter.nzcv == (source & 0xf000'0000U));
		require(write.gadget.nzcv == (source & 0xf000'0000U));
	}
}

void read_only_registers()
{
	for (const auto [instruction, expected] : {
		std::array<std::uint64_t, 2>{0xd53b'd067U, 0U},
		std::array<std::uint64_t, 2>{0xd53b'00e7U, 0x10U},
		std::array<std::uint64_t, 2>{0xd53b'0027U, 0x8444'8004U},
		std::array<std::uint64_t, 2>{0xd53b'e007U, 1'000'000'000U}}) {
		const auto result = run(static_cast<std::uint32_t>(instruction),
			initial_state());
		require(result.interpreter.registers[7] == expected);
		require(result.gadget.registers[7] == expected);
	}

	const auto counter = run(0xd53b'e047U, initial_state());
	require(counter.interpreter.registers[7] != 0);
	require(counter.gadget.registers[7] != 0);

	/* MRS Xt==XZR remains a valid discarded read for every supported source. */
	for (const auto instruction : {0xd53b'd07fU, 0xd53b'00ffU, 0xd53b'003fU,
		0xd53b'e01fU, 0xd53b'e05fU}) {
		const auto initial = initial_state();
		const auto result = run(instruction, initial);
		require(result.interpreter.registers == initial.registers);
		require(result.gadget.registers == initial.registers);
	}
}

void inaccessible_and_reserved()
{
	/* SCTLR_EL1 is inaccessible at EL0 in both directions. Numeric encodings
	 * in the same allocation must take the same early Undefined path. */
	for (const auto instruction : {0xd538'1000U, 0xd518'1000U,
		0xd53f'ffe3U, 0xd51f'ffe3U})
		require_illegal(instruction);

	/* The current EL0 profile exposes no FEAT_SYSREG128 register. Cover both
	 * directions and the architecturally reserved odd-Rt forms. */
	for (const auto instruction : {0xd550'0000U, 0xd570'0000U,
		0xd550'0001U, 0xd570'0001U, 0xd55f'ffe2U, 0xd57f'ffe2U})
		require_illegal(instruction);

	/* FEAT_SYSINSTR128 is likewise absent. Exercise low/high numeric fields and
	 * both a real source and XZR across the SYSP encoding space. */
	for (const unsigned op1 : {0U, 7U})
		for (const unsigned crn : {0U, 8U, 9U, 15U})
			for (const unsigned crm : {0U, 15U})
				for (const unsigned op2 : {0U, 7U})
					for (const unsigned transfer : {0U, 31U})
						require_illegal(0xd548'0000U |
							(op1 << 16) | (crn << 12) | (crm << 8) |
							(op2 << 5) | transfer);
}

void production_specialization()
{
#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
	const auto tls = gadgets::Builder{}.build(pc,
		std::array{0xd53b'd040U, 0xd400'0001U});
	const auto fpcr = gadgets::Builder{}.build(pc,
		std::array{0xd53b'4400U, 0xd400'0001U});
	const auto counter = gadgets::Builder{}.build(pc,
		std::array{0xd53b'e040U, 0xd400'0001U});
	require(tls.program.words().front() != fpcr.program.words().front());
	require(tls.program.words().front() != counter.program.words().front());
	require(fpcr.program.words().front() != counter.program.words().front());
#endif
}

} // namespace

int main()
{
	thread_pointer();
	floating_control_status();
	condition_flags();
	read_only_registers();
	inaccessible_and_reserved();
	production_specialization();
	return 0;
}
