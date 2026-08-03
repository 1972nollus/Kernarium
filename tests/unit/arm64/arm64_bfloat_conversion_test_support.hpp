// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_floating_precision_conversion_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_bfloat_conversion_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;
namespace floating = arm64_floating_precision_conversion_test;

enum class Form : unsigned { scalar, lower, upper };

struct Registers final {
	unsigned destination;
	unsigned source;
};

struct Conversion final {
	std::uint16_t bits;
	std::uint32_t flags;
};

inline constexpr std::array forms{Form::scalar, Form::lower, Form::upper};

[[nodiscard]] constexpr std::uint32_t instruction(Form form,
	Registers registers = {0U, 1U}) noexcept
{
	const std::uint32_t base = form == Form::scalar ? 0x1e63'4000U :
		form == Form::upper ? 0x4ea1'6800U : 0x0ea1'6800U;
	return base | (registers.destination & 31U) |
		((registers.source & 31U) << 5);
}

[[nodiscard]] constexpr Conversion convert(
	std::uint32_t input, std::uint32_t fpcr) noexcept
{
	const bool negative = (input >> 31U) != 0U;
	const unsigned exponent = (input >> 23U) & 0xffU;
	const unsigned fraction = input & 0x7f'ffffU;
	const bool alternative = (fpcr & floating::fpcr_ah) != 0U;
	if (exponent == 0xffU) {
		if (fraction == 0U)
			return {static_cast<std::uint16_t>(input >> 16U), 0U};
		const bool signaling = (fraction & 0x40'0000U) == 0U;
		const auto flags = signaling && !alternative ?
			floating::fpsr_invalid : 0U;
		if ((fpcr & floating::fpcr_dn) != 0U)
			return {static_cast<std::uint16_t>(
				alternative ? 0xffc0U : 0x7fc0U), flags};
		return {static_cast<std::uint16_t>(
			(input >> 16U) | 0x0040U), flags};
	}
	if (exponent == 0U && fraction == 0U)
		return {static_cast<std::uint16_t>(input >> 16U), 0U};

	const bool standard_flush =
		(fpcr & floating::fpcr_fz) != 0U && !alternative;
	const bool input_flush =
		(fpcr & floating::fpcr_fiz) != 0U || alternative;
	if (exponent == 0U && (standard_flush || input_flush))
		return {static_cast<std::uint16_t>(negative ? 0x8000U : 0U),
			standard_flush ? floating::fpsr_input_denormal : 0U};

	const unsigned rounding =
		alternative ? 0U : (fpcr >> 22U) & 3U;
	const auto retained = static_cast<std::uint16_t>(input >> 16U);
	const unsigned discarded = input & 0xffffU;
	if (discarded == 0U)
		return {retained, 0U};
	bool increment{};
	if (rounding == 0U)
		increment = discarded > 0x8000U ||
			(discarded == 0x8000U && (retained & 1U) != 0U);
	else if (rounding == 1U)
		increment = !negative;
	else if (rounding == 2U)
		increment = negative;
	const auto result = static_cast<std::uint16_t>(
		retained + static_cast<unsigned>(increment));
	if (alternative)
		return {result, 0U};
	std::uint32_t flags = floating::fpsr_inexact;
	if (exponent == 0U)
		flags |= floating::fpsr_underflow;
	else if ((result & 0x7f80U) == 0x7f80U &&
		 (result & 0x007fU) == 0U)
		flags |= floating::fpsr_overflow;
	return {result, flags};
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0U)
{
	return floating::state(seed);
}

inline void write_single(std::array<std::byte, 16> &bytes,
	unsigned lane, std::uint32_t bits)
{
	floating::write_lane(bytes, lane, 32U, bits);
}

[[nodiscard]] inline std::uint16_t read_bfloat(
	const std::array<std::byte, 16> &bytes, unsigned lane)
{
	return static_cast<std::uint16_t>(
		floating::read_lane(bytes, lane, 16U));
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements(
	Form form, std::uint32_t fpcr)
{
	using ::uml::cpu::ArmFeature;
	auto requirements =
		engine_tests::NativeRequirements::features(ArmFeature::bf16);
	if ((fpcr & (floating::fpcr_fiz | floating::fpcr_ah)) != 0U ||
	    (form == Form::scalar &&
	     (fpcr & floating::fpcr_nep) != 0U))
		requirements = requirements | ArmFeature::afp;
	return requirements;
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, Form form, Registers registers)
{
	const auto source = initial.vectors[registers.source].bytes;
	auto expected = initial;
	auto result =
		(form == Form::upper ||
		 (form == Form::scalar &&
		  (initial.fpcr & floating::fpcr_nep) != 0U)) ?
		initial.vectors[registers.destination].bytes :
		std::array<std::byte, 16>{};
	const unsigned lanes = form == Form::scalar ? 1U : 4U;
	const unsigned destination_lane = form == Form::upper ? 4U : 0U;
	std::uint32_t flags{};
	for (unsigned lane = 0U; lane < lanes; ++lane) {
		const auto converted = convert(static_cast<std::uint32_t>(
			floating::read_lane(source, lane, 32U)), initial.fpcr);
		floating::write_lane(result, destination_lane + lane, 16U,
			converted.bits);
		flags |= converted.flags;
	}
	expected.vectors[registers.destination].bytes = result;
	expected.fpsr |= flags;
	return expected;
}

inline void check(engine_tests::EngineHarness &harness,
	std::string_view name, Form form, Registers registers,
	const arm64::CpuState &initial)
{
	harness.check_one(name, instruction(form, registers), initial,
		expected_state(initial, form, registers),
		native_requirements(form, initial.fpcr));
}

inline constexpr std::array<std::uint32_t, 20> representative_inputs{
	0x0000'0000U, 0x8000'0000U, 0x0000'0001U, 0x8000'0001U,
	0x0000'ffffU, 0x8000'ffffU, 0x0001'0000U, 0x8001'0000U,
	0x007f'ffffU, 0x807f'ffffU, 0x0080'0000U, 0x8080'0000U,
	0x3f80'0000U, 0xbf80'0000U, 0x7f7f'ffffU, 0xff7f'ffffU,
	0x7f80'0000U, 0xff80'0000U, 0x7fc1'2345U, 0x7f81'2345U,
};

} // namespace arm64_bfloat_conversion_test
