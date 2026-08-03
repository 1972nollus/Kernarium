// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_floating_compare_test_support.hpp"

namespace test = arm64_simd_floating_compare_test;
namespace base = arm64_simd_test;

namespace {

void signed_zero_and_ordered_values(
	test::engine_tests::EngineHarness &harness)
{
	for (const auto shape : test::shapes)
		for (const auto operation : test::zero_operations) {
			auto initial = base::state(shape.width + static_cast<unsigned>(operation));
			const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
			for (unsigned lane = 0; lane < lanes; ++lane) {
				const int relation = static_cast<int>(lane % 4U) - 1;
				const auto value = relation < 0 ?
					test::negative_one(shape.width / 8U) :
					relation > 0 ? test::positive_one(shape.width / 8U) :
					(lane & 1U) != 0U ? test::negative_zero(
						shape.width / 8U) : 0U;
				test::write_lane(initial.vectors[29].bytes, shape, lane, value);
			}
			test::check_result(harness,
				"simd-floating-compare-zero-relations", shape,
				operation, true, 28U, 29U, 0U, initial);
		}
}

void nan_exception_rules(test::engine_tests::EngineHarness &harness)
{
	for (const auto shape : test::shapes)
		for (const auto operation : test::zero_operations)
			for (const bool quiet : {false, true}) {
				auto initial = base::state(shape.width + quiet);
				const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
				for (unsigned lane = 0; lane < lanes; ++lane)
					test::write_lane(initial.vectors[29].bytes, shape, lane,
						quiet ? test::quiet_nan(shape.width / 8U) :
							test::signaling_nan(shape.width / 8U));
				test::check_result(harness,
					"simd-floating-compare-zero-nan", shape,
					operation, true, 28U, 29U, 0U, initial);
			}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	signed_zero_and_ordered_values(harness);
	nan_exception_rules(harness);
	harness.print_summary("arm64-simd-floating-compare-zero");
	return 0;
}
