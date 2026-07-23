// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_floating_compare_test_support.hpp"

namespace test = arm64_simd_floating_compare_test;
namespace base = arm64_simd_test;

namespace {

void ordered_relations_in_every_lane()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const auto operation : {test::Operation::equal,
		     test::Operation::greater_equal, test::Operation::greater}) {
			auto initial = base::state(shape.width + static_cast<unsigned>(operation));
			initial.fpsr = 0x0800'0010U;
			const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
			std::array<bool, 8> expected{};
			for (unsigned lane = 0; lane < lanes; ++lane) {
				const int relation = static_cast<int>(lane % 3U) - 1;
				const auto left = relation < 0 ? test::scalar::negative_one(
					shape.width / 8U) : test::scalar::positive_one(shape.width / 8U);
				const auto right = relation > 0 ? test::scalar::negative_one(
					shape.width / 8U) : test::scalar::positive_one(shape.width / 8U);
				test::write_lane(initial.vectors[29].bytes, shape, lane, left);
				test::write_lane(initial.vectors[30].bytes, shape, lane, right);
				expected[lane] = test::ordered_match(operation, relation);
			}
			test::require_result(test::register_instruction(operation, shape,
				28U, 29U, 30U), shape, operation, false, 28U, 30U,
				std::span{expected}.first(lanes), 0U, initial, harness);
		}
}

void nan_exception_rules()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const auto operation : {test::Operation::equal,
		     test::Operation::greater_equal, test::Operation::greater})
			for (const bool quiet : {false, true}) {
				auto initial = base::state(shape.width + quiet);
				initial.fpsr = 0x0800'0090U;
				const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
				std::array<bool, 8> matches{};
				for (unsigned lane = 0; lane < lanes; ++lane) {
					test::write_lane(initial.vectors[29].bytes, shape, lane,
						quiet ? test::scalar::quiet_nan(shape.width / 8U) :
							test::scalar::signaling_nan(shape.width / 8U));
					test::write_lane(initial.vectors[30].bytes, shape, lane,
						test::scalar::positive_one(shape.width / 8U));
				}
				const bool invalid = !quiet || operation != test::Operation::equal;
				test::require_result(test::register_instruction(operation, shape,
					28U, 29U, 30U), shape, operation, false, 28U, 30U,
					std::span{matches}.first(lanes), invalid ? 1U : 0U,
					initial, harness);
			}
}

} // namespace

int main()
{
	ordered_relations_in_every_lane();
	nan_exception_rules();
	return 0;
}
