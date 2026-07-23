// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvt_vector_test_support.hpp"

namespace test = arm64_simd_fcvt_vector_test;
namespace base = arm64_simd_test;

namespace {

struct ExactCase final {
	test::Shape shape;
	std::array<std::uint64_t, 4> inputs;
	std::array<std::uint64_t, 4> outputs;
};

void exact_finite_special_and_signed_zero_values()
{
	base::Harness harness;
	const std::array cases{
		ExactCase{test::shapes[0],
			{0x3c00U, 0xc000U, 0x7c00U, 0x8000U},
			{0x3f80'0000U, 0xc000'0000U, 0x7f80'0000U, 0x8000'0000U}},
		ExactCase{test::shapes[1],
			{0x3f80'0000U, 0xc000'0000U, 0U, 0U},
			{0x3ff0'0000'0000'0000ULL, 0xc000'0000'0000'0000ULL, 0U, 0U}},
		ExactCase{test::shapes[2],
			{0x3f80'0000U, 0xc000'0000U, 0x7f80'0000U, 0x8000'0000U},
			{0x3c00U, 0xc000U, 0x7c00U, 0x8000U}},
		ExactCase{test::shapes[3],
			{0x3ff0'0000'0000'0000ULL, 0xc000'0000'0000'0000ULL, 0U, 0U},
			{0x3f80'0000U, 0xc000'0000U, 0U, 0U}},
	};
	for (const auto &entry : cases)
		for (const bool upper : {false, true}) {
			auto initial = base::state(entry.shape.source_width + upper);
			const unsigned count = test::lanes(entry.shape);
			for (unsigned lane = 0; lane < count; ++lane)
				test::set_source_lane(initial, 29U, entry.shape, upper, lane,
					entry.inputs[lane]);
			test::require_result(test::instruction(entry.shape, upper,
				28U, 29U), entry.shape, upper, 28U,
				std::span{entry.outputs}.first(count), 0U, initial, harness);
		}
}

} // namespace

int main()
{
	exact_finite_special_and_signed_zero_values();
	return 0;
}
