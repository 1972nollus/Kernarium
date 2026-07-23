// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtzi_test_support.hpp"

namespace test = arm64_simd_fcvtzi_test;
namespace base = arm64_simd_test;

namespace {

void truncates_every_lane_and_accumulates_exceptions()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const bool unsigned_result : {false, true}) {
			auto initial = base::state(shape.width + unsigned_result);
			const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
			std::array<std::uint64_t, 8> outputs{};
			const auto positive = shape.width == 16U ? 0x3e00U :
				shape.width == 32U ? 0x3fc0'0000U :
					0x3ff8'0000'0000'0000ULL;
			for (unsigned lane = 0; lane < lanes; ++lane) {
				const bool negative = !unsigned_result && (lane & 1U) != 0U;
				test::compare::write_lane(initial.vectors[29].bytes,
					shape, lane, positive |
						(negative ? std::uint64_t{1} << (shape.width - 1U) : 0U));
				outputs[lane] = negative ? test::compare::lane_mask(shape.width) : 1U;
			}
			test::require_simd(test::simd_instruction(unsigned_result,
				shape, 28U, 29U), shape, 28U,
				std::span{outputs}.first(lanes), 1U << 4, initial, harness);
		}
}

} // namespace

int main()
{
	truncates_every_lane_and_accumulates_exceptions();
	return 0;
}
