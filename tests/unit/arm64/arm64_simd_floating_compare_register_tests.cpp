// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_floating_compare_test_support.hpp"

namespace test = arm64_simd_floating_compare_test;
namespace base = arm64_simd_test;

namespace {

void ordered_relations_in_every_lane(
	test::engine_tests::EngineHarness &harness)
{
	for (const auto shape : test::shapes)
		for (const auto operation : test::register_operations) {
			auto initial = base::state(shape.width + static_cast<unsigned>(operation));
			initial.fpsr = 0x0800'0010U;
			const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
			for (unsigned lane = 0; lane < lanes; ++lane) {
				const int relation = static_cast<int>(lane % 3U) - 1;
				const auto left = relation < 0 ? test::negative_one(
					shape.width / 8U) : test::positive_one(shape.width / 8U);
				const auto right = relation > 0 ? test::negative_one(
					shape.width / 8U) : test::positive_one(shape.width / 8U);
				test::write_lane(initial.vectors[29].bytes, shape, lane, left);
				test::write_lane(initial.vectors[30].bytes, shape, lane, right);
			}
			test::check_result(harness,
				"simd-floating-compare-register-relations", shape,
				operation, false, 28U, 29U, 30U, initial);
		}
}

void nan_exception_rules(test::engine_tests::EngineHarness &harness)
{
	for (const auto shape : test::shapes)
		for (const auto operation : test::register_operations)
			for (const bool quiet : {false, true})
				for (const bool nan_on_left : {false, true}) {
				auto initial = base::state(shape.width + quiet +
					nan_on_left * 3U);
				initial.fpsr = 0x0800'0090U;
				const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
				for (unsigned lane = 0; lane < lanes; ++lane) {
					const auto nan = quiet ?
						test::quiet_nan(shape.width / 8U) :
						test::signaling_nan(shape.width / 8U);
					test::write_lane(initial.vectors[29].bytes, shape,
						lane, nan_on_left ? nan :
							test::positive_one(shape.width / 8U));
					test::write_lane(initial.vectors[30].bytes, shape, lane,
						nan_on_left ?
							test::positive_one(shape.width / 8U) :
							nan);
				}
				test::check_result(harness,
					"simd-floating-compare-register-nan", shape,
					operation, false, 28U, 29U, 30U, initial);
			}
}

void absolute_magnitude_relations(test::engine_tests::EngineHarness &harness)
{
	for (const auto shape : test::shapes)
		for (const auto operation :
		     {test::Operation::absolute_greater_equal,
		      test::Operation::absolute_greater}) {
			auto initial = base::state(shape.width +
				static_cast<unsigned>(operation));
			const auto bytes = shape.width / 8U;
			const auto &format = test::format_for_bytes(bytes);
			const unsigned lanes = shape.vector_bytes / bytes;
			for (unsigned lane = 0; lane < lanes; ++lane) {
				const unsigned relation = lane % 3U;
				auto left = relation == 0U ? test::positive_one(bytes) :
					test::positive_two(bytes);
				auto right = relation == 2U ? test::positive_one(bytes) :
					test::positive_two(bytes);
				if ((lane & 1U) != 0U)
					left |= format.sign;
				if ((lane & 2U) != 0U)
					right |= format.sign;
				test::write_lane(initial.vectors[29].bytes, shape,
					lane, left);
				test::write_lane(initial.vectors[30].bytes, shape,
					lane, right);
			}
			test::check_result(harness,
				"simd-floating-compare-absolute-magnitudes",
				shape, operation, false, 28U, 29U, 30U,
				initial);
	}
}

void absolute_zero_and_infinity(test::engine_tests::EngineHarness &harness)
{
	for (const auto shape : test::shapes)
		for (const bool infinity : {false, true})
			for (const auto operation :
			     {test::Operation::absolute_greater_equal,
			      test::Operation::absolute_greater}) {
				auto initial = base::state(shape.width + infinity +
					static_cast<unsigned>(operation));
				const auto bytes = shape.width / 8U;
				const auto &format = test::format_for_bytes(bytes);
				const unsigned lanes = shape.vector_bytes / bytes;
				for (unsigned lane = 0; lane < lanes; ++lane) {
					const auto left = infinity ?
						format.sign | format.positive_infinity :
						format.sign;
					const auto right = infinity ?
						format.positive_infinity : 0U;
					test::write_lane(initial.vectors[29].bytes,
						shape, lane, left);
					test::write_lane(initial.vectors[30].bytes,
						shape, lane, right);
				}
				test::check_result(harness,
					"simd-floating-compare-absolute-special",
					shape, operation, false, 28U, 29U, 30U,
					initial);
			}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	ordered_relations_in_every_lane(harness);
	nan_exception_rules(harness);
	absolute_magnitude_relations(harness);
	absolute_zero_and_infinity(harness);
	harness.print_summary("arm64-simd-floating-compare-register");
	return 0;
}
