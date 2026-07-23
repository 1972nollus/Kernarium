// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_facmp_test_support.hpp"

namespace test = arm64_simd_facmp_test;
namespace base = arm64_simd_test;

namespace {

void absolute_relations_in_every_lane()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const auto operation : {test::Operation::greater_equal,
		     test::Operation::greater}) {
			auto initial = base::state(shape.width + static_cast<unsigned>(operation));
			const unsigned bytes = shape.width / 8U;
			const unsigned lanes = shape.vector_bytes / bytes;
			std::array<bool, 8> matches{};
			for (unsigned lane = 0; lane < lanes; ++lane) {
				const unsigned relation = lane % 3U;
				const auto left = relation == 0U ? test::scalar::negative_one(bytes) :
					test::scalar::negative_two(bytes);
				const auto right = relation == 2U ?
					test::scalar::negative_infinity(bytes) :
					test::scalar::positive_one(bytes);
				test::compare::write_lane(initial.vectors[29].bytes, shape,
					lane, left);
				test::compare::write_lane(initial.vectors[30].bytes, shape,
					lane, right);
				matches[lane] = operation == test::Operation::greater_equal ?
					relation != 2U : relation == 1U;
			}
			test::require_result(test::instruction(operation, shape,
				28U, 29U, 30U), shape, operation, 28U, 30U,
				std::span{matches}.first(lanes), 0U, initial, harness);
		}
}

void signed_zero_and_infinity()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const bool infinity : {false, true})
			for (const auto operation : {test::Operation::greater_equal,
			     test::Operation::greater}) {
				auto initial = base::state(shape.width + infinity);
				const unsigned bytes = shape.width / 8U;
				const unsigned lanes = shape.vector_bytes / bytes;
				std::array<bool, 8> matches{};
				for (unsigned lane = 0; lane < lanes; ++lane) {
					const auto left = infinity ? test::scalar::negative_infinity(bytes) :
						test::scalar::negative_zero(bytes);
					const auto right = infinity ? test::scalar::positive_infinity(bytes) :
						0U;
					test::compare::write_lane(initial.vectors[29].bytes, shape,
						lane, left);
					test::compare::write_lane(initial.vectors[30].bytes, shape,
						lane, right);
					matches[lane] = operation == test::Operation::greater_equal;
				}
				test::require_result(test::instruction(operation, shape,
					28U, 29U, 30U), shape, operation, 28U, 30U,
					std::span{matches}.first(lanes), 0U, initial, harness);
			}
}

} // namespace

int main()
{
	absolute_relations_in_every_lane();
	signed_zero_and_infinity();
	return 0;
}
