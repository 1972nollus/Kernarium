// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcmp_test_support.hpp"

#include <array>

namespace test = arm64_simd_fcmp_test;

namespace {

inline constexpr std::array unrelated_control_bits{
	0U,
	1U << 22,
	2U << 22,
	3U << 22,
	1U << 25,
	(3U << 22) | (1U << 25),
};

void half_precision_control_matrix()
{
	test::engine_tests::EngineHarness harness;
	const auto &format = test::formats[0];
	for (unsigned alternative = 0; alternative < 8U; ++alternative)
		for (const bool flush_half : {false, true})
			for (const bool standard_flush : {false, true})
				for (const auto unrelated : unrelated_control_bits)
					for (const bool negative : {false, true})
						for (const bool signaling : {false, true}) {
							auto initial = test::state(
								alternative * 47U +
								flush_half * 31U +
								standard_flush * 23U +
								unrelated + negative * 11U +
								signaling);
							initial.fpcr = alternative |
								(flush_half ? 1U << 19 : 0U) |
								(standard_flush ?
									1U << 24 : 0U) |
								unrelated;
							initial.fpsr = 0x0800'0010U;
							test::put(initial, 29U, format,
								format.minimum_subnormal |
									(negative ?
										format.sign : 0U));
							test::check_direct(harness,
								"FCMP/FCMPE half control state",
								format, signaling, true,
								29U, 0U, initial);
						}
	harness.print_summary("arm64-simd-fcmp-fpcr-half");
}

void single_and_double_control_matrix()
{
	test::engine_tests::EngineHarness harness;
	for (const auto &format : {test::formats[1], test::formats[2]})
		for (unsigned alternative = 0; alternative < 8U; ++alternative)
			for (const bool standard_flush : {false, true})
				for (const auto unrelated : unrelated_control_bits)
					for (const bool negative : {false, true})
						for (const bool signaling : {false, true})
							for (const bool zero : {false, true}) {
								auto initial = test::state(
									format.bytes * 53U +
									alternative * 43U +
									standard_flush * 29U +
									unrelated + negative * 13U +
									signaling * 7U + zero);
								initial.fpcr = alternative |
									(standard_flush ?
										1U << 24 : 0U) |
									unrelated;
								initial.fpsr = 0x0800'0010U;
								test::put(initial, 29U, format,
									format.minimum_subnormal |
										(negative ?
											format.sign : 0U));
								test::put(initial, 30U, format,
									format.minimum_normal);
								test::check_direct(harness,
									"FCMP/FCMPE control state",
									format, signaling, zero,
									29U, 30U, initial);
							}
	harness.print_summary("arm64-simd-fcmp-fpcr-ordinary");
}

void nan_and_subnormal_processing_order()
{
	test::engine_tests::EngineHarness harness;
	for (const auto &format : test::formats)
		for (unsigned alternative = 0; alternative < 4U; ++alternative)
			for (const bool standard_flush : {false, true})
				for (const bool flush_half : {false, true})
					for (const bool signaling : {false, true})
						for (const auto nan : {
							format.quiet_nan,
							format.signaling_nan})
							for (const bool nan_first :
							     {false, true}) {
								auto initial = test::state(
									format.bytes * 59U +
									alternative * 37U +
									standard_flush * 19U +
									flush_half * 13U +
									signaling * 7U +
									nan_first);
								initial.fpcr = alternative |
									(standard_flush ?
										1U << 24 : 0U) |
									(flush_half ?
										1U << 19 : 0U);
								initial.fpsr = 0x0800'0010U;
								test::put(initial, 29U, format,
									nan_first ?
										nan :
										format.minimum_subnormal);
								test::put(initial, 30U, format,
									nan_first ?
										format.minimum_subnormal :
										nan);
								test::check_direct(harness,
									"FCMP/FCMPE NaN processing order",
									format, signaling, false,
									29U, 30U, initial);
							}
	harness.print_summary("arm64-simd-fcmp-fpcr-nan-order");
}

} // namespace

int main()
{
	half_precision_control_matrix();
	single_and_double_control_matrix();
	nan_and_subnormal_processing_order();
	return 0;
}
