// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvt_vector_test_support.hpp"

namespace test = arm64_simd_fcvt_vector_test;
namespace base = arm64_simd_test;

namespace {

void every_pair_half_and_alias()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const bool upper : {false, true})
			for (const unsigned destination : {28U, 29U}) {
				auto initial = base::state(shape.source_width + upper + destination);
				const unsigned count = test::lanes(shape);
				std::array<std::uint64_t, 4> outputs{};
				for (unsigned lane = 0; lane < count; ++lane) {
					test::set_source_lane(initial, 29U, shape, upper, lane,
						test::scalar::one(shape.source_width));
					outputs[lane] = test::scalar::one(shape.destination_width);
				}
				test::require_result(test::instruction(shape, upper,
					destination, 29U), shape, upper, destination,
					std::span{outputs}.first(count), 0U, initial, harness);
			}
}

} // namespace

int main()
{
	every_pair_half_and_alias();
	return 0;
}
