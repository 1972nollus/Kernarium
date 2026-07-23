// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvt_vector_test_support.hpp"

namespace test = arm64_simd_fcvt_vector_test;
namespace base = arm64_simd_test;

namespace {

void all_rounding_modes_for_both_narrow_pairs()
{
	base::Harness harness;
	struct Entry final {
		test::Shape shape;
		std::uint64_t input;
		std::array<std::uint64_t, 4> outputs;
	};
	const Entry entries[]{
		{test::shapes[2], 0x3f80'1000U,
			{0x3c00U, 0x3c01U, 0x3c00U, 0x3c00U}},
		{test::shapes[3], 0x3ff0'0000'1000'0000ULL,
			{0x3f80'0000U, 0x3f80'0001U, 0x3f80'0000U,
				0x3f80'0000U}},
	};
	for (const auto &entry : entries)
		for (const bool upper : {false, true})
			for (unsigned mode = 0; mode < 4U; ++mode) {
				auto initial = base::state(entry.shape.source_width + mode + upper);
				initial.fpcr = mode << 22;
				initial.fpsr = 0x0800'0080U;
				const unsigned count = test::lanes(entry.shape);
				std::array<std::uint64_t, 4> outputs{};
				for (unsigned lane = 0; lane < count; ++lane) {
					test::set_source_lane(initial, 29U, entry.shape, upper,
						lane, entry.input);
					outputs[lane] = entry.outputs[mode];
				}
				test::require_result(test::instruction(entry.shape, upper,
					28U, 29U), entry.shape, upper, 28U,
					std::span{outputs}.first(count), 1U << 4, initial, harness);
			}
}

void underflow_overflow_and_flags_are_sticky()
{
	base::Harness harness;
	const auto shape = test::shapes[3];
	for (const bool upper : {false, true}) {
		auto overflow = base::state(70U + upper);
		const unsigned count = test::lanes(shape);
		std::array<std::uint64_t, 4> infinity{};
		for (unsigned lane = 0; lane < count; ++lane) {
			test::set_source_lane(overflow, 29U, shape, upper, lane,
				0x7fe0'0000'0000'0000ULL);
			infinity[lane] = 0x7f80'0000U;
		}
		test::require_result(test::instruction(shape, upper, 28U, 29U),
			shape, upper, 28U, std::span{infinity}.first(count),
			(1U << 2) | (1U << 4), overflow, harness);

		auto underflow = base::state(80U + upper);
		std::array<std::uint64_t, 4> zero{};
		for (unsigned lane = 0; lane < count; ++lane)
			test::set_source_lane(underflow, 29U, shape, upper, lane,
				0x3690'0000'0000'0000ULL);
		test::require_result(test::instruction(shape, upper, 28U, 29U),
			shape, upper, 28U, std::span{zero}.first(count),
			(1U << 3) | (1U << 4), underflow, harness);
	}
}

} // namespace

int main()
{
	all_rounding_modes_for_both_narrow_pairs();
	underflow_overflow_and_flags_are_sticky();
	return 0;
}
