// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcmp_test_support.hpp"

namespace test = arm64_simd_fcmp_test;

namespace {

void every_condition_input_fallback_and_signaling_mode()
{
	test::engine_tests::EngineHarness harness;
	for (const auto &format : test::formats)
		for (const bool signaling : {false, true})
			for (unsigned condition = 0; condition < 16U; ++condition)
				for (unsigned flags = 0; flags < 16U; ++flags)
					for (unsigned fallback = 0;
					     fallback < 16U; ++fallback) {
						auto initial = test::state(
							format.bytes * 37U +
							signaling * 23U +
							condition * 17U +
							flags * 11U + fallback);
						initial.nzcv = flags << 28;
						initial.fpsr =
							0x0800'0010U | (fallback & 1U);
						test::put(initial, 29U, format,
							format.positive_one |
								((fallback & 1U) ?
									format.sign : 0U));
						test::put(initial, 30U, format,
							format.positive_two);
						test::check_conditional(harness,
							"FCCMP/FCCMPE conditions",
							format, signaling, condition,
							fallback, 29U, 30U, initial);
					}
	harness.print_summary("arm64-simd-fcmp-conditional-matrix");
}

void failed_conditions_do_not_process_operands()
{
	test::engine_tests::EngineHarness harness;
	for (const auto &format : test::formats)
		for (const bool signaling : {false, true})
			for (unsigned condition = 0; condition < 14U; ++condition) {
				unsigned failing_flags = 0U;
				while (test::condition_holds(
					failing_flags << 28, condition))
					++failing_flags;
				for (const auto left : {
					format.quiet_nan, format.signaling_nan})
					for (const auto right : {
						format.minimum_subnormal,
						format.signaling_nan}) {
						auto initial = test::state(
							format.bytes * 41U +
							signaling * 19U +
							condition * 13U +
							static_cast<unsigned>(left));
						initial.nzcv = failing_flags << 28;
						initial.fpcr = format.bytes == 2U ?
							(1U << 19) :
							(1U << 24) | 2U;
						initial.fpsr = 0x0800'0010U;
						test::put(initial, 29U, format, left);
						test::put(initial, 30U, format, right);
						test::check_conditional(harness,
							"FCCMP/FCCMPE failed condition",
							format, signaling, condition,
							13U, 29U, 30U, initial);
					}
			}
	harness.print_summary("arm64-simd-fcmp-conditional-short-circuit");
}

void passed_conditions_process_both_nan_classes()
{
	test::engine_tests::EngineHarness harness;
	for (const auto &format : test::formats)
		for (const bool signaling : {false, true})
			for (unsigned condition = 0; condition < 16U; ++condition) {
				unsigned passing_flags = 0U;
				while (!test::condition_holds(
					passing_flags << 28, condition))
					++passing_flags;
				for (const auto left : {
					format.quiet_nan, format.signaling_nan})
					for (const auto right : {
						format.positive_one,
						format.quiet_nan,
						format.signaling_nan}) {
						auto initial = test::state(
							format.bytes * 43U +
							signaling * 29U +
							condition * 7U +
							static_cast<unsigned>(right));
						initial.nzcv = passing_flags << 28;
						initial.fpsr = 0x0800'0090U;
						test::put(initial, 29U, format, left);
						test::put(initial, 30U, format, right);
						test::check_conditional(harness,
							"FCCMP/FCCMPE passed NaN",
							format, signaling, condition,
							9U, 29U, 30U, initial);
					}
			}
	harness.print_summary("arm64-simd-fcmp-conditional-nan");
}

} // namespace

int main()
{
	every_condition_input_fallback_and_signaling_mode();
	failed_conditions_do_not_process_operands();
	passed_conditions_process_both_nan_classes();
	return 0;
}
