// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_multiply_test_support.hpp"

namespace test = arm64_bfloat_multiply_test;

namespace {

[[nodiscard]] test::common::arm64::CpuState one_lane(std::uint32_t addend,
	std::uint16_t left, std::uint16_t right, std::uint32_t fpcr)
{
	auto initial = test::common::state(300);
	initial.fpcr = fpcr;
	initial.fpsr = 0U;
	for (unsigned lane = 0; lane < 4U; ++lane) {
		test::set_single(initial, 0, lane, addend);
		test::set_bfloat(initial, 1, lane * 2U, left);
		test::set_bfloat(initial, 2, lane * 2U, right);
	}
	return initial;
}

} // namespace

int main()
{
	test::common::Harness harness;
	const auto operation = test::instruction(test::Form::vector_bottom);

	/* The ordinary path reports invalid zero-times-infinity. */
	auto invalid = one_lane(0U, 0U, 0x7f80U, 0U);
	auto invalid_result = harness.run_interpreter(operation, invalid);
	test::common::require(test::single_lane(invalid_result, 0, 0) ==
		0x7fc0'0000U);
	test::common::require((invalid_result.fpsr & 1U) != 0U);
	harness.require_differential(operation, invalid);

	/* AH suppresses exceptions and gives its signed default NaN. */
	auto alternative_invalid = one_lane(0U, 0U, 0x7f80U, 2U);
	auto alternative_invalid_result = harness.run_interpreter(
		operation, alternative_invalid);
	test::common::require(test::single_lane(
		alternative_invalid_result, 0, 0) == 0xffc0'0000U);
	test::common::require(alternative_invalid_result.fpsr == 0U);
	harness.require_differential(operation, alternative_invalid);

	/* AH three-NaN priority selects the first multiplicand ([n]). */
	auto nan_priority = one_lane(0x7fc1'0000U, 0x7fc2U, 0x7fc3U, 2U);
	auto nan_priority_result = harness.run_interpreter(operation, nan_priority);
	test::common::require(test::single_lane(nan_priority_result, 0, 0) ==
		0x7fc2'0000U);
	test::common::require(nan_priority_result.fpsr == 0U);
	harness.require_differential(operation, nan_priority);

	auto default_nan = nan_priority;
	default_nan.fpcr |= 1U << 25;
	auto default_nan_result = harness.run_interpreter(operation, default_nan);
	test::common::require(test::single_lane(default_nan_result, 0, 0) ==
		0xffc0'0000U);
	harness.require_differential(operation, default_nan);

	/* 2^-24 is exactly halfway between 1.0 and its binary32 successor. */
	const auto half_ulp_left = test::bfloat(0x1p-12F);
	auto upward = one_lane(0x3f80'0000U, half_ulp_left, half_ulp_left,
		1U << 22);
	auto upward_result = harness.run_interpreter(operation, upward);
	test::common::require(test::single_lane(upward_result, 0, 0) ==
		0x3f80'0001U);
	test::common::require((upward_result.fpsr & (1U << 4)) != 0U);
	harness.require_differential(operation, upward);

	auto forced_nearest = upward;
	forced_nearest.fpcr |= 2U;
	auto forced_nearest_result = harness.run_interpreter(operation,
		forced_nearest);
	test::common::require(test::single_lane(forced_nearest_result, 0, 0) ==
		0x3f80'0000U);
	test::common::require(forced_nearest_result.fpsr == 0U);
	harness.require_differential(operation, forced_nearest);

	/* FIZ flushes without IDC; FZ flushes the same input and reports IDC. */
	auto fiz = one_lane(0U, 1U, test::bfloat(1.0F), 1U);
	auto fiz_result = harness.run_interpreter(operation, fiz);
	test::common::require(test::single_lane(fiz_result, 0, 0) == 0U);
	test::common::require((fiz_result.fpsr & (1U << 7)) == 0U);
	harness.require_differential(operation, fiz);
	auto fz = fiz;
	fz.fpcr = 1U << 24;
	auto fz_result = harness.run_interpreter(operation, fz);
	test::common::require(test::single_lane(fz_result, 0, 0) == 0U);
	test::common::require((fz_result.fpsr & (1U << 7)) != 0U);
	harness.require_differential(operation, fz);
}
