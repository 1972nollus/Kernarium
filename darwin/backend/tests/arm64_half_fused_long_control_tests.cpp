// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_fused_long_test_support.hpp"

namespace test = arm64_half_fused_long_test;

namespace {

[[nodiscard]] test::common::arm64::CpuState one_value(std::uint32_t addend,
	std::uint16_t left, std::uint16_t right, std::uint32_t fpcr)
{
	auto initial = test::common::state(300);
	initial.fpcr = fpcr;
	initial.fpsr = 0U;
	for (unsigned lane = 0; lane < 4U; ++lane) {
		test::set_single(initial, 0, lane, addend);
		test::set_half(initial, 1, lane, left);
		test::set_half(initial, 2, lane, right);
	}
	return initial;
}

} // namespace

int main()
{
	test::common::Harness harness;
	const auto add = test::instruction({false, false, false, true, 0U});
	const auto subtract = test::instruction({false, true, false, true, 0U});
	const auto tiny = test::half(0x1p-12F);
	auto nearest = one_value(0x3f80'0000U, tiny, tiny, 0U);
	auto nearest_result = harness.run_interpreter(add, nearest);
	test::common::require(test::single_lane(nearest_result, 0, 0) ==
		0x3f80'0000U);
	test::common::require((nearest_result.fpsr & (1U << 4)) != 0U);
	harness.require_differential(add, nearest);
	auto upward = nearest;
	upward.fpcr = 1U << 22;
	auto upward_result = harness.run_interpreter(add, upward);
	test::common::require(test::single_lane(upward_result, 0, 0) ==
		0x3f80'0001U);
	harness.require_differential(add, upward);

	/* FZ16 flushes half inputs without raising IDC. */
	auto half_denormal = one_value(0U, 1U, test::half(1.0F), 1U << 19);
	auto half_denormal_result = harness.run_interpreter(add, half_denormal);
	test::common::require(test::single_lane(half_denormal_result, 0, 0) == 0U);
	test::common::require((half_denormal_result.fpsr & (1U << 7)) == 0U);
	harness.require_differential(add, half_denormal);

	/* Ordinary invalid and signaling NaN cases report IOC. */
	auto invalid = one_value(0U, 0U, 0x7c00U, 0U);
	auto invalid_result = harness.run_interpreter(add, invalid);
	test::common::require(test::single_lane(invalid_result, 0, 0) ==
		0x7fc0'0000U);
	test::common::require((invalid_result.fpsr & 1U) != 0U);
	harness.require_differential(add, invalid);

	/* AH uses [n] NaN priority, preserves its sign under FPNeg, and still IOC. */
	auto alternative_nan = one_value(0x7fc1'0000U, 0xfc01U, 0x7e03U, 2U);
	auto alternative_nan_result = harness.run_interpreter(
		subtract, alternative_nan);
	test::common::require(test::single_lane(alternative_nan_result, 0, 0) ==
		0xffc0'2000U);
	test::common::require((alternative_nan_result.fpsr & 1U) != 0U);
	harness.require_differential(subtract, alternative_nan);

	/* FIZ flushes an addend denormal without IDC; AH without FIZ reports IDC. */
	auto fiz = one_value(1U, 0U, test::half(1.0F), 1U);
	auto fiz_result = harness.run_interpreter(add, fiz);
	test::common::require(test::single_lane(fiz_result, 0, 0) == 0U);
	test::common::require((fiz_result.fpsr & (1U << 7)) == 0U);
	harness.require_differential(add, fiz);
	auto ah_denormal = fiz;
	ah_denormal.fpcr = 2U;
	auto ah_denormal_result = harness.run_interpreter(add, ah_denormal);
	test::common::require(test::single_lane(ah_denormal_result, 0, 0) == 1U);
	test::common::require((ah_denormal_result.fpsr & (1U << 7)) != 0U);
	harness.require_differential(add, ah_denormal);
}
