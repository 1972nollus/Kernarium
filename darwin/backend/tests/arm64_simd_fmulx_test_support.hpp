// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_simd_fp_indexed_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace arm64_simd_fmulx_test {

namespace fp = arm64_simd_fp_indexed_test;
namespace engine_tests = uml::backend::tests;

[[nodiscard]] constexpr std::uint32_t instruction(const fp::Shape &shape,
	unsigned destination, unsigned left, unsigned right)
{
	const std::uint32_t operation = shape.size == 0U ?
		(shape.scalar ? 0x5e40'1c00U : 0x0e40'1c00U) :
		(shape.scalar ? 0x5e20'dc00U : 0x0e20'dc00U);
	return operation | (!shape.scalar && shape.q ? 0x4000'0000U : 0U) |
		(shape.size == 3U ? 0x0040'0000U : 0U) |
		((right & 31U) << 16) | ((left & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] inline engine_tests::NativeRequirements native_requirements(
	const fp::Shape &shape, const uml::backend::arm64::CpuState &initial)
{
	using uml::backend::ArmFeature;
	auto requirements =
		engine_tests::NativeRequirements::features(ArmFeature::advanced_simd);
	if (shape.lane_bytes == 2U)
		requirements = requirements | ArmFeature::fp16;
	if ((initial.fpcr & 6U) != 0U)
		requirements = requirements | ArmFeature::afp;
	return requirements;
}

[[nodiscard]] inline uml::backend::arm64::CpuState expected_uniform(
	const uml::backend::arm64::CpuState &initial, const fp::Shape &shape,
	unsigned destination, unsigned left, std::uint64_t result_bits,
	std::uint32_t expected_fpsr)
{
	auto expected = initial;
	std::array<std::byte, 16> result{};
	if (shape.scalar && (initial.fpcr & 4U) != 0U)
		result = initial.vectors[left].bytes;
	const unsigned bytes =
		shape.scalar ? shape.lane_bytes : shape.q ? 16U : 8U;
	for (unsigned lane = 0; lane < bytes / shape.lane_bytes; ++lane)
		fp::write_bits(result, shape.lane_bytes, lane, result_bits);
	expected.vectors[destination].bytes = result;
	expected.fpsr = expected_fpsr;
	return expected;
}

inline void check_uniform(engine_tests::EngineHarness &harness,
	std::string_view name, const fp::Shape &shape, unsigned destination,
	unsigned left, unsigned right,
	const uml::backend::arm64::CpuState &initial, std::uint64_t result_bits,
	std::uint32_t expected_fpsr)
{
	harness.check_one(name,
		instruction(shape, destination, left, right), initial,
		expected_uniform(initial, shape, destination, left, result_bits,
			expected_fpsr),
		native_requirements(shape, initial));
}

} // namespace arm64_simd_fmulx_test
