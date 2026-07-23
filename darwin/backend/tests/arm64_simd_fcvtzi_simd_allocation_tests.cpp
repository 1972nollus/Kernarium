// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtzi_test_support.hpp"

namespace test = arm64_simd_fcvtzi_test;
namespace base = arm64_simd_test;

namespace {

void every_shape_signedness_alias_and_nep()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const bool unsigned_result : {false, true})
			for (const unsigned destination : {28U, 29U}) {
				auto initial = base::state(shape.width + destination + unsigned_result);
				if (shape.scalar)
					initial.fpcr = 4U;
				const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
				std::array<std::uint64_t, 8> outputs{};
				for (unsigned lane = 0; lane < lanes; ++lane) {
					test::compare::write_lane(initial.vectors[29].bytes,
						shape, lane, test::precision::one(shape.width));
					outputs[lane] = 1U;
				}
				test::require_simd(test::simd_instruction(unsigned_result,
					shape, destination, 29U), shape, destination,
					std::span{outputs}.first(lanes), 0U, initial, harness);
			}
}

void reserved_short_double_vector_is_early_illegal()
{
	base::Harness harness;
	const test::Shape reserved{64U, 8U, false, false};
	for (const bool unsigned_result : {false, true})
		harness.require_illegal(test::simd_instruction(unsigned_result,
			reserved, 28U, 29U), base::state(91U));
}

} // namespace

int main()
{
	every_shape_signedness_alias_and_nep();
	reserved_short_double_vector_is_early_illegal();
	return 0;
}
