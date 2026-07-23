// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_matrix_test_support.hpp"

namespace test = arm64_half_matrix_test;

namespace {

[[nodiscard]] test::common::arm64::CpuState empty_state(unsigned seed,
	std::uint32_t fpcr)
{
	auto state = test::common::state(seed);
	test::clear_vectors(state);
	state.fpcr = fpcr;
	state.fpsr = 0;
	return state;
}

} // namespace

int main()
{
	test::common::Harness harness;
	const auto narrow = test::instruction(false);
	const auto wide = test::instruction(true);

	auto rounding = empty_state(300, 0U);
	test::set_half(rounding, 1, 0, 0x3c01U);
	test::set_half(rounding, 2, 0, 0x3c01U);
	auto nearest = harness.run_interpreter(narrow, rounding);
	test::common::require(test::half_lane(nearest, 0, 0) == 0x3c02U);
	test::common::require((nearest.fpsr & (1U << 4)) != 0U);
	harness.require_differential(narrow, rounding);
	rounding.fpcr = 1U << 22;
	auto plus = harness.run_interpreter(narrow, rounding);
	test::common::require(test::half_lane(plus, 0, 0) == 0x3c03U);
	harness.require_differential(narrow, rounding);

	auto nan = empty_state(301, 2U);
	test::set_half(nan, 1, 0, 0x7e01U);
	test::set_half(nan, 2, 0, 0x7c02U);
	auto nan_result = harness.run_interpreter(narrow, nan);
	test::common::require(test::half_lane(nan_result, 0, 0) == 0x7e01U);
	test::common::require((nan_result.fpsr & 1U) != 0U);
	harness.require_differential(narrow, nan);

	auto flush_half = empty_state(302, 1U << 19);
	test::set_half(flush_half, 1, 0, 0x0400U);
	test::set_half(flush_half, 2, 0, 0x3800U);
	auto standard_flush = harness.run_interpreter(narrow, flush_half);
	test::common::require(test::half_lane(standard_flush, 0, 0) == 0U);
	test::common::require((standard_flush.fpsr & ((1U << 3) | (1U << 4))) ==
		(1U << 3));
	harness.require_differential(narrow, flush_half);
	flush_half.fpcr |= 2U;
	auto alternative_flush = harness.run_interpreter(narrow, flush_half);
	test::common::require(test::half_lane(alternative_flush, 0, 0) == 0U);
	test::common::require((alternative_flush.fpsr &
		((1U << 3) | (1U << 4))) == ((1U << 3) | (1U << 4)));
	harness.require_differential(narrow, flush_half);
	auto flush_input = empty_state(306, 2U | (1U << 19));
	test::set_half(flush_input, 1, 0, 0x0001U);
	test::set_half(flush_input, 2, 0, 0x7c00U);
	auto flush_input_result = harness.run_interpreter(narrow, flush_input);
	test::common::require(test::half_lane(flush_input_result, 0, 0) ==
		0xfe00U);
	test::common::require((flush_input_result.fpsr & 1U) != 0U);
	harness.require_differential(narrow, flush_input);

	auto fiz = empty_state(303, 1U);
	test::set_single(fiz, 0, 0, 1U);
	auto fiz_result = harness.run_interpreter(wide, fiz);
	test::common::require(test::single_lane(fiz_result, 0, 0) == 0U);
	test::common::require((fiz_result.fpsr & (1U << 7)) == 0U);
	harness.require_differential(wide, fiz);

	auto ah_fz = empty_state(304, 2U | (1U << 24));
	test::set_single(ah_fz, 0, 0, 1U);
	auto ah_fz_result = harness.run_interpreter(wide, ah_fz);
	test::common::require(test::single_lane(ah_fz_result, 0, 0) == 0U);
	test::common::require((ah_fz_result.fpsr &
		((1U << 7) | (1U << 3) | (1U << 4))) ==
		((1U << 7) | (1U << 3) | (1U << 4)));
	harness.require_differential(wide, ah_fz);

	auto dot_nan = empty_state(305, 2U | (1U << 25));
	test::set_half(dot_nan, 1, 0, 0x7c01U);
	test::set_half(dot_nan, 2, 0, test::half(1.0F));
	auto dot_nan_result = harness.run_interpreter(wide, dot_nan);
	test::common::require(test::single_lane(dot_nan_result, 0, 0) ==
		0xffc0'0000U);
	test::common::require((dot_nan_result.fpsr & 1U) != 0U);
	harness.require_differential(wide, dot_nan);
}
