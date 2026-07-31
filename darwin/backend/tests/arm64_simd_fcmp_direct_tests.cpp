// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcmp_test_support.hpp"

#include <array>

namespace test = arm64_simd_fcmp_test;

namespace {

[[nodiscard]] constexpr std::array<std::uint64_t, 16> values(
	const test::Format &format)
{
	return {
		0U,
		format.sign,
		format.positive_one,
		format.positive_one | format.sign,
		format.positive_two,
		format.positive_two | format.sign,
		format.positive_infinity,
		format.positive_infinity | format.sign,
		format.maximum_finite,
		format.maximum_finite | format.sign,
		format.minimum_normal,
		format.minimum_normal | format.sign,
		format.minimum_subnormal,
		format.minimum_subnormal | format.sign,
		format.quiet_nan,
		format.signaling_nan,
	};
}

void every_value_class_pair_and_form()
{
	test::engine_tests::EngineHarness harness;
	for (const auto &format : test::formats)
		for (const bool signaling : {false, true})
			for (const bool zero : {false, true})
				for (const auto left_value : values(format))
					for (const auto right_value : values(format)) {
						auto initial = test::state(
							format.bytes * 29U +
							signaling * 17U + zero * 11U +
							static_cast<unsigned>(left_value) +
							static_cast<unsigned>(right_value));
						initial.fpsr =
							0x0800'0010U |
							(static_cast<std::uint32_t>(
								left_value ^ right_value) &
							 0x1fU);
						test::put(initial, 29U, format,
							left_value);
						test::put(initial, 30U, format,
							right_value);
						test::check_direct(harness,
							"FCMP/FCMPE value classes",
							format, signaling, zero, 29U,
							30U, initial);
					}
	harness.print_summary("arm64-simd-fcmp-direct-values");
}

void both_nan_classes_and_register_aliases()
{
	test::engine_tests::EngineHarness harness;
	for (const auto &format : test::formats)
		for (const bool signaling : {false, true})
			for (const auto left : {
				format.quiet_nan, format.signaling_nan})
				for (const auto right : {
					format.quiet_nan, format.signaling_nan})
					for (unsigned reg = 0; reg < 32U; ++reg) {
						auto initial = test::state(
							format.bytes * 31U + reg * 7U +
							signaling);
						initial.fpsr = 0x0800'0090U;
						test::put(initial, reg, format,
							left == right ? left : right);
						test::check_direct(harness,
							"FCMP/FCMPE both NaNs and alias",
							format, signaling, false, reg,
							reg, initial);
					}
	harness.print_summary("arm64-simd-fcmp-direct-nan-alias");
}

} // namespace

int main()
{
	every_value_class_pair_and_form();
	both_nan_classes_and_register_aliases();
	return 0;
}
