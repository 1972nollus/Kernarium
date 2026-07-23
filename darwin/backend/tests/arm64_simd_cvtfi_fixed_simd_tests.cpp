// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_cvtfi_test_support.hpp"

namespace test = arm64_simd_cvtfi_test;
namespace base = arm64_simd_test;

int main()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const bool unsigned_source : {false, true})
			for (unsigned fractional_bits = 1U;
			     fractional_bits <= shape.width; ++fractional_bits) {
				auto initial = base::state(shape.width + fractional_bits);
				const unsigned lanes =
					shape.vector_bytes / (shape.width / 8U);
				std::array<std::uint64_t, 8> output{};
				for (unsigned lane = 0; lane < lanes; ++lane)
					test::set_lane(initial, 29U, shape.width, lane, 0U);
				test::require_vector(test::simd_fixed_instruction(
					unsigned_source, shape, fractional_bits, 28U, 29U),
					shape.width, shape.vector_bytes, shape.scalar, 28U,
					std::span{output}.first(lanes), 0U, initial, harness);
			}
	return 0;
}
