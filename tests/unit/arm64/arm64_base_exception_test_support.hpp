// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_exception_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

[[nodiscard]] inline arm64::CpuState state()
{
	arm64::CpuState cpu{};
	for (std::size_t index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			0xa5a5'5a5a'1234'0000ULL | static_cast<std::uint64_t>(index);
	for (std::size_t reg = 0; reg < cpu.vectors.size(); ++reg)
		for (std::size_t byte = 0;
		     byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] =
				static_cast<std::byte>(reg * 17U + byte * 29U);
	cpu.sp = 0xfeed'face'cafe'beefULL;
	cpu.nzcv = 0xb000'0000U;
	cpu.fpcr = 0x0400'0000U;
	cpu.fpsr = 0x0800'0000U;
	cpu.fpmr = 0x0000'0012'3400'0041ULL;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	cpu.set_mte_configuration(true, 0x8421U);
	cpu.set_mte_generator(true, 0xace1U, 7,
		0xfeed'face'0123'4567ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = 0xa1b2'c3d4U;
	cpu.pstate_ssbs = true;
	cpu.pstate_dit = true;
	cpu.pstate_tco = true;
	cpu.pstate_sm = true;
	cpu.pstate_za = true;
	cpu.pstate_daif_access = true;
	cpu.pstate_daif = 0xaU;
	cpu.exclusive_address = 0x7654'3000ULL;
	cpu.exclusive_key = 0x1234U;
	cpu.exclusive_value = 0xa5a5'a5a5'a5a5'a5a5ULL;
	cpu.exclusive_value2 = 0x5a5a'5a5a'5a5a'5a5aULL;
	cpu.exclusive_version = 19;
	cpu.exclusive_size = 16;
	return cpu;
}

[[nodiscard]] constexpr std::uint32_t with_immediate(
	std::uint32_t base_instruction, unsigned immediate)
{
	return base_instruction | ((immediate & 0xffffU) << 5);
}

class Harness final {
public:
	void check(std::string_view name, std::uint32_t instruction,
		engine_tests::ExpectedEvent event, bool advances,
		bool invalid_encoding = false)
	{
		const std::array code{
			engine_tests::ProgramWord{
				.offset = 0,
				.instruction = instruction,
			},
		};
		const auto initial = state();
		auto expected = initial;
		expected.pc = engine_tests::engine_test_code_base +
			(advances ? sizeof(std::uint32_t) : 0U);
		engines_.check_program({
			.name = name,
			.code = std::span<const engine_tests::ProgramWord>{code},
			.initial = initial,
			.expected = expected,
			.event = event,
			.native = engine_tests::NativeRequirements::baseline(),
			.invalid_encoding = invalid_encoding,
		});
	}

	void print_summary(std::string_view name) const
	{
		engines_.print_summary(name);
	}

private:
	engine_tests::EngineHarness engines_;
};

} // namespace arm64_exception_test
