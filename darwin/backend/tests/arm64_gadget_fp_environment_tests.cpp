// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <source_location>

namespace arm64 = uml::backend::arm64;
namespace gadgets = arm64::gadgets;

namespace {

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (condition)
		return;
	std::fprintf(stderr, "require failed at %s:%u\n",
		location.file_name(), location.line());
	std::abort();
}

void set_single(arm64::CpuState &cpu, unsigned index, std::uint32_t bits)
{
	std::memcpy(cpu.vectors[index].bytes.data(), &bits, sizeof(bits));
}

std::uint32_t single(const arm64::CpuState &cpu, unsigned index)
{
	std::uint32_t bits = 0;
	std::memcpy(&bits, cpu.vectors[index].bytes.data(), sizeof(bits));
	return bits;
}

template<std::size_t Size>
arm64::CpuState execute(const std::array<std::uint32_t, Size> &instructions,
	arm64::CpuState cpu)
{
	const auto built = gadgets::Builder{}.build(0x8000, instructions);
	require(built.complete);
	gadgets::Tlb tlb;
	require(gadgets::execute(built.program, cpu, tlb) ==
		gadgets::Event::system_call);
	return cpu;
}

} // namespace

int main()
{
	/* Activate the native environment, change FPCR while it is active, then
	 * prove both the register source and XZR write affect the very next native
	 * operation. 1.0 + half an ULP distinguishes +Infinity from nearest. */
	{
		const std::array instructions{
			0x1e22'2824U, // fadd s4,s1,s2 (activates default environment)
			0xd51b'4403U, // msr fpcr,x3
			0x1e22'2820U, // fadd s0,s1,s2
			0xd51b'441fU, // msr fpcr,xzr
			0x1e22'2825U, // fadd s5,s1,s2
			0xd400'0001U, // svc #0
		};
		arm64::CpuState initial{};
		initial.registers[3] = 1U << 22; // Round toward plus infinity.
		set_single(initial, 1, 0x3f80'0000U);
		set_single(initial, 2, 0x3380'0000U);
		const auto cpu = execute(instructions, initial);
		require(single(cpu, 4) == 0x3f80'0000U);
		require(single(cpu, 0) == 0x3f80'0001U);
		require(single(cpu, 5) == 0x3f80'0000U);
		require(cpu.fpcr == 0);
	}

	/* An active FPSR write must update native status immediately; otherwise
	 * leave would resurrect the divide-by-zero flag cleared by MSR FPSR. */
	{
		const std::array instructions{
			0x1e22'1820U, // fdiv s0,s1,s2
			0xd51b'443fU, // msr fpsr,xzr
			0x1e22'2823U, // fadd s3,s1,s2
			0xd53b'4424U, // mrs x4,fpsr
			0xd400'0001U, // svc #0
		};
		arm64::CpuState initial{};
		set_single(initial, 1, 0x3f80'0000U);
		set_single(initial, 2, 0);
		const auto cpu = execute(instructions, initial);
		require(cpu.registers[4] == 0);
		require(cpu.fpsr == 0);
	}
}
