// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_basic_floating_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace arm64_fused_floating_test {

namespace basic = arm64_basic_floating_test;
namespace common = arm64_simd_test;
namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

struct Form final {
	bool scalar;
	bool negate;
	bool subtract;
	unsigned width;
	unsigned vector_bytes;
};

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
	unsigned addend;
};

inline constexpr std::array register_cases{
	Registers{0, 1, 2, 3}, Registers{1, 1, 2, 3},
	Registers{2, 1, 2, 3}, Registers{3, 1, 2, 3},
	Registers{1, 1, 1, 1}, Registers{31, 30, 29, 28},
};

template<typename Function>
void for_each_form(Function &&function)
{
	for (const bool negate : {false, true})
		for (const bool subtract : {false, true})
			for (const auto width : basic::widths)
				function(Form{true, negate, subtract, width, 16U});
	for (const bool subtract : {false, true})
		for (const auto shape : basic::vector_shapes)
			function(Form{false, false, subtract, shape[0], shape[1]});
}

[[nodiscard]] constexpr std::uint32_t instruction(const Form &form,
	Registers registers = {0, 1, 2, 3}) noexcept
{
	std::uint32_t value;
	if (form.scalar) {
		value = 0x1f00'0000U |
			(basic::scalar_type(form.width) << 22) |
			(form.negate ? 0x0020'0000U : 0U) |
			(form.subtract ? 0x0000'8000U : 0U) |
			((registers.addend & 31U) << 10);
	} else {
		const bool q = form.vector_bytes == 16U;
		if (form.width == 16U)
			value = 0x0e40'0c00U;
		else
			value = 0x0e20'cc00U |
				(form.width == 64U ? 0x0040'0000U : 0U);
		value |= q ? 0x4000'0000U : 0U;
		value |= form.subtract ? 0x0080'0000U : 0U;
	}
	return value | ((registers.right & 31U) << 16) |
		((registers.left & 31U) << 5) | (registers.destination & 31U);
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(const Form &form)
{
	using uml::backend::ArmFeature;
	auto requirements = engine_tests::NativeRequirements::features(
		form.scalar ? ArmFeature::floating_point :
			ArmFeature::advanced_simd);
	return form.width == 16U ?
		requirements | ArmFeature::fp16 : requirements;
}

[[nodiscard]] constexpr unsigned active_lanes(const Form &form) noexcept
{
	return form.scalar ? 1U :
		form.vector_bytes / (form.width / 8U);
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0)
{
	return basic::state(seed);
}

inline void set_lane_bits(arm64::CpuState &cpu, unsigned reg,
	unsigned lane, unsigned width, std::uint64_t bits)
{
	basic::set_lane_bits(cpu, reg, lane, width, bits);
}

inline void fill_active_lanes(arm64::CpuState &cpu, unsigned reg,
	const Form &form, std::uint64_t bits)
{
	for (unsigned lane = 0; lane < active_lanes(form); ++lane)
		set_lane_bits(cpu, reg, lane, form.width, bits);
}

[[nodiscard]] inline arm64::CpuState expected_uniform(
	const arm64::CpuState &initial, const Form &form,
	Registers registers, std::uint64_t result_bits,
	std::uint32_t new_fpsr_flags = 0)
{
	auto expected = initial;
	std::array<std::byte, 16> result{};
	if (form.scalar && (initial.fpcr & 4U) != 0U)
		result = initial.vectors[registers.addend].bytes;
	const unsigned lane_bytes = form.width / 8U;
	for (unsigned lane = 0; lane < active_lanes(form); ++lane)
		for (unsigned byte = 0; byte < lane_bytes; ++byte)
			result[lane * lane_bytes + byte] = static_cast<std::byte>(
				(result_bits >> (byte * 8U)) & 0xffU);
	expected.vectors[registers.destination].bytes = result;
	expected.fpsr |= new_fpsr_flags;
	return expected;
}

inline void check_uniform(engine_tests::EngineHarness &harness,
	std::string_view name, const Form &form, Registers registers,
	const arm64::CpuState &initial, std::uint64_t result_bits,
	std::uint32_t new_fpsr_flags = 0)
{
	harness.check_one(name, instruction(form, registers), initial,
		expected_uniform(initial, form, registers, result_bits,
			new_fpsr_flags),
		native_requirements(form));
}

struct ExpectedCounts final {
	std::size_t cases{};
	std::size_t jit{};
	std::size_t feature_skips{};

	void add(engine_tests::NativeRequirements requirements,
		bool invalid_encoding = false)
	{
		++cases;
		if (invalid_encoding || engine_tests::host_supports(requirements))
			++jit;
		else
			++feature_skips;
	}
};

inline void require_counts(const engine_tests::EngineHarness &harness,
	const ExpectedCounts &expected)
{
	const auto &actual = harness.counts();
	common::require(actual.interpreter == expected.cases);
	common::require(actual.gadgets == expected.cases);
	common::require(actual.jit == expected.jit);
	common::require(actual.jit_skipped_host_feature ==
		expected.feature_skips);
	common::require(actual.jit_skipped == expected.feature_skips);
	common::require(actual.jit_unavailable == 0U);
}

template<typename Floating>
void prepare_exact(arm64::CpuState &cpu, const Form &form,
	Registers registers)
{
	const unsigned lanes = form.scalar ? 1U :
		form.vector_bytes / sizeof(Floating);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		basic::set_lane(cpu, registers.left, lane,
			static_cast<Floating>(1.5));
		basic::set_lane(cpu, registers.right, lane,
			static_cast<Floating>(2.0));
		basic::set_lane(cpu, form.scalar ? registers.addend :
			registers.destination, lane, static_cast<Floating>(4.0));
	}
}

template<typename Floating>
[[nodiscard]] Floating exact_result(const Form &form)
{
	const auto product = static_cast<Floating>(3.0);
	const auto addend = static_cast<Floating>(4.0);
	if (!form.negate)
		return form.subtract ? addend - product : addend + product;
	return form.subtract ? product - addend : -(addend + product);
}

} // namespace arm64_fused_floating_test
