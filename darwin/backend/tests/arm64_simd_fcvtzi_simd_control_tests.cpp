// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtzi_test_support.hpp"

namespace test = arm64_simd_fcvtzi_test;
namespace base = arm64_simd_test;

namespace {

void nan_infinity_saturation_and_half_control()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const bool unsigned_result : {false, true}) {
			auto initial = base::state(shape.width + unsigned_result);
			const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
			std::array<std::uint64_t, 8> outputs{};
			for (unsigned lane = 0; lane < lanes; ++lane)
				test::compare::write_lane(initial.vectors[29].bytes,
					shape, lane, test::precision::quiet_nan(shape.width));
			test::require_simd(test::simd_instruction(unsigned_result,
				shape, 28U, 29U), shape, 28U,
				std::span{outputs}.first(lanes), 1U, initial, harness);
		}

	for (const auto shape : {test::shapes[0], test::shapes[3], test::shapes[4]}) {
		auto initial = base::state(shape.vector_bytes);
		initial.fpcr = 1U << 26;
		const unsigned lanes = shape.vector_bytes / 2U;
		std::array<std::uint64_t, 8> outputs{};
		for (unsigned lane = 0; lane < lanes; ++lane) {
			test::compare::write_lane(initial.vectors[29].bytes,
				shape, lane, 0x7c00U);
			outputs[lane] = 0x7fffU;
		}
		test::require_simd(test::simd_instruction(false, shape, 28U, 29U),
			shape, 28U, std::span{outputs}.first(lanes), 1U,
			initial, harness);
	}
}

} // namespace

int main()
{
	nan_infinity_saturation_and_half_control();
	return 0;
}
