// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtzi_test_support.hpp"

namespace test = arm64_simd_fcvtzi_test;
namespace base = arm64_simd_test;

namespace {

void every_rounding_shape_and_signedness()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const auto rounding : test::roundings)
			for (const bool unsigned_result : {false, true}) {
				auto initial = base::state(shape.width +
					static_cast<unsigned>(rounding) + unsigned_result);
				const unsigned lanes =
					shape.vector_bytes / (shape.width / 8U);
				std::array<std::uint64_t, 8> outputs{};
				for (unsigned lane = 0; lane < lanes; ++lane) {
					test::compare::write_lane(initial.vectors[29].bytes,
						shape, lane,
						test::floating_bits(shape.width, false,
							(lane & 1U) != 0U));
					outputs[lane] = test::expected_rounded(rounding, false,
						(lane & 1U) != 0U, shape.width);
				}
				test::require_simd(test::simd_instruction(rounding,
					unsigned_result, shape, 28U, 29U), shape, 28U,
					std::span{outputs}.first(lanes), 1U << 4,
					initial, harness);
			}
}

void signed_negative_lanes()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const auto rounding : test::roundings) {
			auto initial = base::state(shape.width +
				static_cast<unsigned>(rounding));
			const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
			std::array<std::uint64_t, 8> outputs{};
			for (unsigned lane = 0; lane < lanes; ++lane) {
				test::compare::write_lane(initial.vectors[29].bytes,
					shape, lane, test::floating_bits(shape.width, true, false));
				outputs[lane] = test::expected_rounded(rounding, true, false,
					shape.width);
			}
			test::require_simd(test::simd_instruction(rounding, false,
				shape, 28U, 29U), shape, 28U,
				std::span{outputs}.first(lanes), 1U << 4, initial, harness);
		}
}

} // namespace

int main()
{
	every_rounding_shape_and_signedness();
	signed_negative_lanes();
	return 0;
}
