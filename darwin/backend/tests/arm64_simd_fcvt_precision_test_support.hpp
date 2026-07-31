// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_floating_precision_conversion_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_simd_fcvt_precision_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;
namespace oracle = arm64_floating_precision_conversion_test;

struct Pair final {
	unsigned source_width;
	unsigned destination_width;
};

struct Registers final {
	unsigned destination;
	unsigned source;
};

inline constexpr std::array pairs{
	Pair{16U, 32U}, Pair{16U, 64U}, Pair{32U, 16U},
	Pair{32U, 64U}, Pair{64U, 16U}, Pair{64U, 32U},
};

[[nodiscard]] constexpr unsigned type_for_width(unsigned width) noexcept
{
	return width == 16U ? 3U : width == 32U ? 0U : 1U;
}

[[nodiscard]] constexpr std::uint32_t instruction(Pair pair,
	Registers registers = {28U, 29U}) noexcept
{
	return 0x1e22'4000U |
		(type_for_width(pair.source_width) << 22) |
		(type_for_width(pair.destination_width) << 15) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t instruction_from_types(
	unsigned source_type, unsigned destination_type,
	Registers registers = {28U, 29U}) noexcept
{
	return 0x1e22'4000U | ((source_type & 3U) << 22) |
		((destination_type & 3U) << 15) |
		((registers.source & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] constexpr bool valid_type_pair(unsigned source_type,
	unsigned destination_type) noexcept
{
	return source_type != 2U && destination_type != 2U &&
		source_type != destination_type;
}

[[nodiscard]] constexpr bool belongs_to_bfloat_conversion(
	unsigned source_type, unsigned destination_type) noexcept
{
	return source_type == 1U && destination_type == 2U;
}

[[nodiscard]] constexpr std::uint64_t one(unsigned width) noexcept
{
	return width == 16U ? 0x3c00U :
		width == 32U ? 0x3f80'0000U :
			0x3ff0'0000'0000'0000ULL;
}

[[nodiscard]] constexpr std::uint64_t infinity(unsigned width,
	bool negative = false) noexcept
{
	return oracle::infinity_bits(width, negative);
}

[[nodiscard]] constexpr std::uint64_t quiet_nan(unsigned width,
	bool negative = false) noexcept
{
	const auto value = width == 16U ? 0x7e55U :
		width == 32U ? 0x7fc1'2345U :
			0x7ff8'1234'5678'9abcULL;
	return value | (negative ? oracle::sign_mask(width) : 0U);
}

[[nodiscard]] constexpr std::uint64_t signaling_nan(unsigned width,
	bool negative = false) noexcept
{
	const auto value = width == 16U ? 0x7d51U :
		width == 32U ? 0x7f81'2345U :
			0x7ff0'1234'5678'9abcULL;
	return value | (negative ? oracle::sign_mask(width) : 0U);
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0U)
{
	return oracle::state(seed);
}

inline void set_source(arm64::CpuState &cpu, unsigned source,
	unsigned source_width, std::uint64_t bits)
{
	oracle::write_lane(cpu.vectors[source].bytes, 0U, source_width, bits);
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements(
	std::uint32_t fpcr)
{
	using uml::backend::ArmFeature;
	auto requirements =
		engine_tests::NativeRequirements::features(
			ArmFeature::floating_point);
	if ((fpcr & (oracle::fpcr_fiz | oracle::fpcr_ah |
		     oracle::fpcr_nep)) != 0U)
		requirements = requirements | ArmFeature::afp;
	return requirements;
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, Pair pair, Registers registers)
{
	const auto source = initial.vectors[registers.source].bytes;
	auto expected = initial;
	auto result = (initial.fpcr & oracle::fpcr_nep) != 0U ?
		initial.vectors[registers.destination].bytes :
		std::array<std::byte, 16>{};
	const auto conversion = oracle::convert(
		oracle::read_lane(source, 0U, pair.source_width),
		pair.source_width, pair.destination_width, initial.fpcr);
	oracle::write_lane(result, 0U, pair.destination_width,
		conversion.bits);
	expected.vectors[registers.destination].bytes = result;
	expected.fpsr |= conversion.flags;
	return expected;
}

inline void check(engine_tests::EngineHarness &harness,
	std::string_view name, Pair pair, Registers registers,
	const arm64::CpuState &initial)
{
	harness.check_one(name, instruction(pair, registers), initial,
		expected_state(initial, pair, registers),
		native_requirements(initial.fpcr));
}

inline void check_illegal(engine_tests::EngineHarness &harness,
	std::string_view name, unsigned source_type,
	unsigned destination_type, Registers registers,
	const arm64::CpuState &initial)
{
	harness.check_illegal(name,
		instruction_from_types(source_type, destination_type, registers),
		initial, native_requirements(initial.fpcr));
}

} // namespace arm64_simd_fcvt_precision_test
