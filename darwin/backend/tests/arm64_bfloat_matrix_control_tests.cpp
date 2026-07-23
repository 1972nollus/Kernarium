// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_matrix_test_support.hpp"

namespace test = arm64_bfloat_matrix_test;

namespace {

[[nodiscard]] test::common::arm64::CpuState matrix_state(std::uint32_t addend,
	const std::array<std::uint16_t, 8> &left,
	const std::array<std::uint16_t, 8> &right, std::uint32_t fpcr)
{
	auto initial = test::common::state(200);
	initial.fpcr = fpcr;
	initial.fpsr = 0x0800'009fU;
	for (unsigned lane = 0; lane < 4U; ++lane)
		test::dot::set_single(initial, 0, lane, addend);
	for (unsigned lane = 0; lane < 8U; ++lane) {
		test::dot::set_bfloat(initial, 1, lane, left[lane]);
		test::dot::set_bfloat(initial, 2, lane, right[lane]);
	}
	return initial;
}

} // namespace

int main()
{
	test::common::Harness harness;
	const auto operation = test::instruction();
	const auto half_ulp = test::dot::bfloat(0x1p-12F);
	std::array<std::uint16_t, 8> left{};
	std::array<std::uint16_t, 8> right{};
	left[0] = half_ulp;
	right[0] = half_ulp;
	auto standard = matrix_state(0x3f80'0000U, left, right, 0U);
	auto standard_result = harness.run_interpreter(operation, standard);
	test::common::require(test::dot::single_lane(standard_result, 0, 0) ==
		0x3f80'0001U);
	test::common::require(standard_result.fpsr == standard.fpsr);
	harness.require_differential(operation, standard);

	auto extended = standard;
	extended.fpcr = 1U << 13;
	auto extended_result = harness.run_interpreter(operation, extended);
	test::common::require(test::dot::single_lane(extended_result, 0, 0) ==
		0x3f80'0000U);
	test::common::require(extended_result.fpsr == extended.fpsr);
	harness.require_differential(operation, extended);

	left.fill(0x7f7fU);
	right = {test::dot::bfloat(2.0F), test::dot::bfloat(-2.0F),
		test::dot::bfloat(2.0F), test::dot::bfloat(-2.0F),
		test::dot::bfloat(2.0F), test::dot::bfloat(-2.0F),
		test::dot::bfloat(2.0F), test::dot::bfloat(-2.0F)};
	auto cancellation = matrix_state(0U, left, right, 0U);
	auto standard_cancel = harness.run_interpreter(operation, cancellation);
	test::common::require(test::dot::single_lane(standard_cancel, 0, 0) ==
		0x7fc0'0000U);
	harness.require_differential(operation, cancellation);
	cancellation.fpcr = 1U << 13;
	auto extended_cancel = harness.run_interpreter(operation, cancellation);
	test::common::require(test::dot::single_lane(extended_cancel, 0, 0) == 0U);
	harness.require_differential(operation, cancellation);

	left.fill(0U);
	right.fill(0U);
	left[0] = 0x7f81U;
	right[0] = test::dot::bfloat(1.0F);
	auto nan = matrix_state(0U, left, right, (1U << 13) | 2U);
	auto nan_result = harness.run_interpreter(operation, nan);
	test::common::require(test::dot::single_lane(nan_result, 0, 0) ==
		0xffc0'0000U);
	test::common::require(nan_result.fpsr == nan.fpsr);
	harness.require_differential(operation, nan);
}
