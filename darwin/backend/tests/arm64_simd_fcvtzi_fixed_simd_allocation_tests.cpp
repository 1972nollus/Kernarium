// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtzi_test_support.hpp"

namespace test = arm64_simd_fcvtzi_test;
namespace base = arm64_simd_test;

namespace {

void every_shape_signedness_scale_alias_and_nep()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const bool unsigned_result : {false, true})
			for (unsigned fractional_bits = 1U;
			     fractional_bits <= shape.width; ++fractional_bits)
				for (const unsigned destination : {28U, 29U}) {
					auto initial = base::state(shape.width + fractional_bits +
						destination + unsigned_result);
					if (shape.scalar)
						initial.fpcr = 4U;
					const unsigned lanes =
						shape.vector_bytes / (shape.width / 8U);
					std::array<std::uint64_t, 8> outputs{};
					for (unsigned lane = 0; lane < lanes; ++lane)
						test::compare::write_lane(initial.vectors[29].bytes,
							shape, lane, 0U);
					test::require_simd(test::simd_fixed_instruction(
						unsigned_result, shape, fractional_bits,
						destination, 29U), shape, destination,
						std::span{outputs}.first(lanes), 0U,
						initial, harness);
				}
}

} // namespace

int main()
{
	every_shape_signedness_scale_alias_and_nep();
	return 0;
}
