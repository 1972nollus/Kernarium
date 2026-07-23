// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_dot_test_support.hpp"

namespace test = arm64_bfloat_dot_test;

namespace {

[[nodiscard]] test::common::arm64::CpuState dot_state(std::uint32_t addend,
	std::uint16_t left_a, std::uint16_t left_b, std::uint16_t right_a,
	std::uint16_t right_b, std::uint32_t fpcr)
{
	auto initial = test::common::state(300);
	initial.fpcr = fpcr;
	initial.fpsr = 0x0800'009fU;
	for (unsigned pair = 0; pair < 4U; ++pair) {
		test::set_single(initial, 0, pair, addend);
		test::set_bfloat(initial, 1, pair * 2U, left_a);
		test::set_bfloat(initial, 1, pair * 2U + 1U, left_b);
		test::set_bfloat(initial, 2, pair * 2U, right_a);
		test::set_bfloat(initial, 2, pair * 2U + 1U, right_b);
	}
	return initial;
}

} // namespace

int main()
{
	test::common::Harness harness;
	const auto operation = test::instruction({false, true, 0U});
	const auto half_ulp = test::bfloat(0x1p-12F);

	/* Standard BF16 mode uses fixed round-to-odd, independent of RMode. */
	auto standard = dot_state(0x3f80'0000U, half_ulp, 0U,
		half_ulp, 0U, 2U << 22);
	auto standard_result = harness.run_interpreter(operation, standard);
	test::common::require(test::single_lane(standard_result, 0, 0) ==
		0x3f80'0001U);
	test::common::require(standard_result.fpsr == standard.fpsr);
	harness.require_differential(operation, standard);

	/* EBF uses ordinary RNE for the accumulation into the destination. */
	auto extended = standard;
	extended.fpcr = 1U << 13;
	auto extended_result = harness.run_interpreter(operation, extended);
	test::common::require(test::single_lane(extended_result, 0, 0) ==
		0x3f80'0000U);
	test::common::require(extended_result.fpsr == extended.fpsr);
	harness.require_differential(operation, extended);

	auto extended_up = extended;
	extended_up.fpcr |= 1U << 22;
	auto extended_up_result = harness.run_interpreter(operation, extended_up);
	test::common::require(test::single_lane(extended_up_result, 0, 0) ==
		0x3f80'0001U);
	harness.require_differential(operation, extended_up);

	/* EBF fuses the two products before their one rounding. */
	auto cancellation = dot_state(0U, 0x7f7fU, 0x7f7fU,
		test::bfloat(2.0F), test::bfloat(-2.0F), 0U);
	auto standard_cancel = harness.run_interpreter(operation, cancellation);
	test::common::require(test::single_lane(standard_cancel, 0, 0) ==
		0x7fc0'0000U);
	harness.require_differential(operation, cancellation);
	cancellation.fpcr = 1U << 13;
	auto extended_cancel = harness.run_interpreter(operation, cancellation);
	test::common::require(test::single_lane(extended_cancel, 0, 0) == 0U);
	harness.require_differential(operation, cancellation);

	/* DN is forced, exceptions stay untouched, and AH controls default sign. */
	auto nan = dot_state(0U, 0x7f81U, 0U, test::bfloat(1.0F), 0U,
		(1U << 13) | 2U);
	auto nan_result = harness.run_interpreter(operation, nan);
	test::common::require(test::single_lane(nan_result, 0, 0) ==
		0xffc0'0000U);
	test::common::require(nan_result.fpsr == nan.fpsr);
	harness.require_differential(operation, nan);

	/* Standard mode always flushes BFloat16 denormals with no FPSR change. */
	auto denormal = dot_state(0U, 1U, 0U, test::bfloat(1.0F), 0U, 0U);
	auto denormal_result = harness.run_interpreter(operation, denormal);
	test::common::require(test::single_lane(denormal_result, 0, 0) == 0U);
	test::common::require(denormal_result.fpsr == denormal.fpsr);
	harness.require_differential(operation, denormal);
}
