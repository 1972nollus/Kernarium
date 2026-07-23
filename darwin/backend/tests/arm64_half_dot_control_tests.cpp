// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_dot_test_support.hpp"

namespace test = arm64_half_dot_test;

namespace {

[[nodiscard]] test::common::arm64::CpuState empty_state(unsigned seed,
	std::uint32_t fpcr)
{
	auto state = test::common::state(seed);
	test::matrix::clear_vectors(state);
	state.fpcr = fpcr;
	state.fpsr = 0;
	return state;
}

} // namespace

int main()
{
	test::common::Harness harness;
	const auto operation = test::instruction({false, false, 0});

	auto nan = empty_state(300, 2U);
	test::matrix::set_half(nan, 1, 0, 0x7e01U);
	test::matrix::set_half(nan, 1, 1, 0x7c02U);
	test::matrix::set_half(nan, 2, 0, test::matrix::half(1.0F));
	test::matrix::set_half(nan, 2, 1, test::matrix::half(1.0F));
	auto nan_result = harness.run_interpreter(operation, nan);
	test::common::require(test::matrix::single_lane(nan_result, 0, 0) ==
		0x7fc0'4000U);
	test::common::require((nan_result.fpsr & 1U) != 0U);
	harness.require_differential(operation, nan);

	auto fiz = empty_state(301, 1U);
	test::matrix::set_single(fiz, 0, 0, 1U);
	auto fiz_result = harness.run_interpreter(operation, fiz);
	test::common::require(test::matrix::single_lane(fiz_result, 0, 0) == 0U);
	test::common::require((fiz_result.fpsr & (1U << 7)) == 0U);
	harness.require_differential(operation, fiz);

	auto ah_fz = empty_state(302, 2U | (1U << 24));
	test::matrix::set_single(ah_fz, 0, 0, 1U);
	auto ah_fz_result = harness.run_interpreter(operation, ah_fz);
	test::common::require(test::matrix::single_lane(ah_fz_result, 0, 0) == 0U);
	test::common::require((ah_fz_result.fpsr &
		((1U << 7) | (1U << 3) | (1U << 4))) ==
		((1U << 7) | (1U << 3) | (1U << 4)));
	harness.require_differential(operation, ah_fz);

	auto fz16 = empty_state(303, 1U << 19);
	test::matrix::set_half(fz16, 1, 0, 1U);
	test::matrix::set_half(fz16, 2, 0, 0x7c00U);
	auto fz16_result = harness.run_interpreter(operation, fz16);
	test::common::require(test::matrix::single_lane(fz16_result, 0, 0) ==
		0x7fc0'0000U);
	test::common::require((fz16_result.fpsr & 1U) != 0U);
	harness.require_differential(operation, fz16);
}
