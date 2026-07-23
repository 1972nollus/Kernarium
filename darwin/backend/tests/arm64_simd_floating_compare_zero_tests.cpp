// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_floating_compare_test_support.hpp"

namespace test = arm64_simd_floating_compare_test;
namespace base = arm64_simd_test;

namespace {

void signed_zero_and_ordered_values()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const auto operation : {test::Operation::equal,
		     test::Operation::greater_equal, test::Operation::greater,
		     test::Operation::less_equal, test::Operation::less}) {
			auto initial = base::state(shape.width + static_cast<unsigned>(operation));
			const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
			std::array<bool, 8> matches{};
			for (unsigned lane = 0; lane < lanes; ++lane) {
				const int relation = static_cast<int>(lane % 4U) - 1;
				const auto value = relation < 0 ?
					test::scalar::negative_one(shape.width / 8U) :
					relation > 0 ? test::scalar::positive_one(shape.width / 8U) :
					(lane & 1U) != 0U ? test::scalar::negative_zero(
						shape.width / 8U) : 0U;
				test::write_lane(initial.vectors[29].bytes, shape, lane, value);
				matches[lane] = test::ordered_match(operation,
					relation < 0 ? -1 : relation > 0 ? 1 : 0);
			}
			test::require_result(test::zero_instruction(operation, shape,
				28U, 29U), shape, operation, true, 28U, 0U,
				std::span{matches}.first(lanes), 0U, initial, harness);
		}
}

void nan_exception_rules()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const auto operation : {test::Operation::equal,
		     test::Operation::greater_equal, test::Operation::greater,
		     test::Operation::less_equal, test::Operation::less})
			for (const bool quiet : {false, true}) {
				auto initial = base::state(shape.width + quiet);
				const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
				std::array<bool, 8> matches{};
				for (unsigned lane = 0; lane < lanes; ++lane)
					test::write_lane(initial.vectors[29].bytes, shape, lane,
						quiet ? test::scalar::quiet_nan(shape.width / 8U) :
							test::scalar::signaling_nan(shape.width / 8U));
				const bool invalid = !quiet || operation != test::Operation::equal;
				test::require_result(test::zero_instruction(operation, shape,
					28U, 29U), shape, operation, true, 28U, 0U,
					std::span{matches}.first(lanes), invalid ? 1U : 0U,
					initial, harness);
			}
}

} // namespace

int main()
{
	signed_zero_and_ordered_values();
	nan_exception_rules();
	return 0;
}
