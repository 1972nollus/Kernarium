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

constexpr std::uint64_t pc = 0x4b0'0000;

class NullMemory final : public arm64::GuestMemory {
public:
	[[nodiscard]] bool read(std::uint64_t, std::span<std::byte>) override
	{
		++accesses;
		return false;
	}
	[[nodiscard]] bool write(
		std::uint64_t, std::span<const std::byte>) override
	{
		++accesses;
		return false;
	}
	std::size_t accesses{};
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

[[nodiscard]] constexpr std::uint32_t pstate(
	unsigned op1, unsigned immediate, unsigned op2)
{
	return 0xd500'401fU | ((op1 & 7U) << 16) |
		((immediate & 15U) << 8) | ((op2 & 7U) << 5);
}

[[nodiscard]] arm64::CpuState state(bool daif_access = false)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] = 0x1020'3040'5060'7080ULL ^ index;
	cpu.sp = 0x1122'3344'5566'7780ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
	cpu.tls = 0x8877'6655'4433'2211ULL;
	cpu.pstate_ssbs = true;
	cpu.pstate_dit = false;
	cpu.pstate_tco = true;
	cpu.pstate_sm = false;
	cpu.pstate_za = true;
	cpu.pstate_daif_access = daif_access;
	cpu.pstate_daif = 0xa;
	return cpu;
}

void require_core_equal(const arm64::CpuState &actual,
	const arm64::CpuState &expected, std::uint64_t expected_pc)
{
	require(actual.registers == expected.registers);
	require(actual.sp == expected.sp && actual.pc == expected_pc);
	require(actual.nzcv == expected.nzcv && actual.fpcr == expected.fpcr &&
		actual.fpsr == expected.fpsr && actual.tls == expected.tls);
	require(actual.pstate_ssbs == expected.pstate_ssbs);
	require(actual.pstate_dit == expected.pstate_dit);
	require(actual.pstate_tco == expected.pstate_tco);
	require(actual.pstate_sm == expected.pstate_sm);
	require(actual.pstate_za == expected.pstate_za);
	require(actual.pstate_daif_access == expected.pstate_daif_access);
	require(actual.pstate_daif == expected.pstate_daif);
}

void run_success(std::uint32_t instruction, const arm64::CpuState &initial,
	const arm64::CpuState &expected)
{
	NullMemory memory;
	auto interpreted = initial;
	interpreted.pc = pc;
	require(arm64::Dispatcher{}.execute(
		interpreted, memory, instruction).reason ==
		arm64::StopReason::continued);
	require_core_equal(interpreted, expected, pc + 4);
	require(memory.accesses == 0);

	const std::array stream{instruction, 0xd400'0001U};
	const auto built = gadgets::Builder{}.build(pc, stream);
	require(built.complete);
	auto gadget = initial;
	gadget.pc = pc;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, gadget, *tlb) ==
		gadgets::Event::system_call);
	require_core_equal(gadget, expected, pc + 8);
}

void run_illegal(std::uint32_t instruction, const arm64::CpuState &initial)
{
	NullMemory memory;
	auto interpreted = initial;
	interpreted.pc = pc;
	const auto result = arm64::Dispatcher{}.execute(
		interpreted, memory, instruction);
	require(result.reason == arm64::StopReason::illegal_instruction);
	require(result.address == pc && result.instruction == instruction);
	require_core_equal(interpreted, initial, pc);
	require(memory.accesses == 0);

	const std::array stream{instruction, 0xd400'0001U};
	const auto built = gadgets::Builder{}.build(pc, stream);
	require(built.complete);
	auto gadget = initial;
	gadget.pc = pc;
	auto tlb = std::make_unique<gadgets::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, gadget, *tlb) ==
		gadgets::Event::illegal_instruction);
	require_core_equal(gadget, initial, pc);
}

[[nodiscard]] bool expected_state(arm64::CpuState &expected,
	unsigned op1, unsigned immediate, unsigned op2)
{
	if (op1 != 3)
		return false;
	const bool value = (immediate & 1U) != 0;
	switch (op2) {
	case 1: expected.pstate_ssbs = value; return true;
	case 2: expected.pstate_dit = value; return true;
	case 3:
		switch ((immediate >> 1) & 3U) {
		case 1: expected.pstate_sm = value; return true;
		case 2: expected.pstate_za = value; return true;
		case 3:
			expected.pstate_sm = value;
			expected.pstate_za = value;
			return true;
		default: return false;
		}
	case 4: expected.pstate_tco = value; return true;
	case 6:
		if (!expected.pstate_daif_access)
			return false;
		expected.pstate_daif = static_cast<std::uint8_t>(
			(expected.pstate_daif | immediate) & 15U);
		return true;
	case 7:
		if (!expected.pstate_daif_access)
			return false;
		expected.pstate_daif = static_cast<std::uint8_t>(
			expected.pstate_daif & ~immediate & 15U);
		return true;
	default: return false;
	}
}

void exhaustive_encodings()
{
	for (unsigned op1 = 0; op1 < 8; ++op1)
		for (unsigned op2 = 0; op2 < 8; ++op2)
			for (unsigned immediate = 0; immediate < 16; ++immediate) {
				/* These three exact encodings are CFINV/XAFLAG/AXFLAG and
				 * belong to their dedicated instruction-family tests. */
				if (op1 == 0 && immediate == 0 && op2 <= 2)
					continue;
				const auto instruction = pstate(op1, immediate, op2);
				auto initial = state(false);
				auto expected = initial;
				if (expected_state(expected, op1, immediate, op2))
					run_success(instruction, initial, expected);
				else
					run_illegal(instruction, initial);

				if (op1 == 3 && (op2 == 6 || op2 == 7)) {
					initial = state(true);
					expected = initial;
					require(expected_state(
						expected, op1, immediate, op2));
					run_success(instruction, initial, expected);
				}
			}
}

void streaming_aliases()
{
	/* CRm[2:1] selects SM, ZA, or both; CRm[0] selects SMSTART/SMSTOP. */
	for (unsigned field = 1; field <= 3; ++field)
		for (const bool start : {false, true}) {
			const unsigned immediate = field * 2U + start;
			const auto initial = state();
			auto expected = initial;
			require(expected_state(expected, 3, immediate, 3));
			run_success(pstate(3, immediate, 3), initial, expected);
		}
}

void production_specialization()
{
#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
	const auto ssbs = gadgets::Builder{}.build(
		pc, std::array{pstate(3, 1, 1)});
	const auto dit = gadgets::Builder{}.build(
		pc, std::array{pstate(3, 1, 2)});
	const auto both = gadgets::Builder{}.build(
		pc, std::array{pstate(3, 7, 3)});
	const auto daif = gadgets::Builder{}.build(
		pc, std::array{pstate(3, 1, 6)});
	/* Scalar fields deliberately share one direct-offset store gadget. The
	 * dual-state and gated DAIF operations are different native gadgets. */
	require(ssbs.program.words().front() == dit.program.words().front());
	require(ssbs.program.words().front() != both.program.words().front());
	require(ssbs.program.words().front() != daif.program.words().front());
	require(both.program.words().front() != daif.program.words().front());
#endif
}

} // namespace

int main()
{
	exhaustive_encodings();
	streaming_aliases();
	production_specialization();
	return 0;
}
