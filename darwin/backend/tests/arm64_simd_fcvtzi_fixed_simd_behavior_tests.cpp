// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtzi_test_support.hpp"

namespace test = arm64_simd_fcvtzi_test;
namespace base = arm64_simd_test;

namespace {

void scaled_lane_values()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const bool unsigned_result : {false, true})
			for (const unsigned fractional_bits : {1U, 8U}) {
				auto initial = base::state(shape.width + fractional_bits +
					unsigned_result);
				const unsigned lanes =
					shape.vector_bytes / (shape.width / 8U);
				std::array<std::uint64_t, 8> outputs{};
				const std::uint64_t expected =
					std::uint64_t{3} << (fractional_bits - 1U);
				for (unsigned lane = 0; lane < lanes; ++lane) {
					test::compare::write_lane(initial.vectors[29].bytes,
						shape, lane,
						test::floating_bits(shape.width, false, false));
					outputs[lane] = expected;
				}
				test::require_simd(test::simd_fixed_instruction(
					unsigned_result, shape, fractional_bits, 28U, 29U),
					shape, 28U, std::span{outputs}.first(lanes), 0U,
					initial, harness);
			}
}

} // namespace

int main()
{
	scaled_lane_values();
	return 0;
}
