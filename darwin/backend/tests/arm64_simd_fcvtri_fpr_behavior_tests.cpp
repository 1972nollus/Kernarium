// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtzi_test_support.hpp"

namespace test = arm64_simd_fcvtzi_test;
namespace base = arm64_simd_test;

namespace {

void every_cross_width_rounding()
{
	base::Harness harness;
	for (const auto widths : {std::pair{16U, 32U}, std::pair{16U, 64U},
			std::pair{32U, 64U}, std::pair{64U, 32U}})
		for (const auto rounding : test::roundings)
			for (const bool unsigned_result : {false, true})
				for (const bool tie : {false, true}) {
					auto initial = base::state(widths.first + widths.second +
						static_cast<unsigned>(rounding) + tie);
					test::set_gpr_source(initial, 29U, widths.first,
						test::floating_bits(widths.first, false, tie));
					test::require_fpr(test::fpr_instruction(rounding,
						unsigned_result, widths.first, widths.second,
						28U, 29U), widths.second, 28U,
						test::expected_rounded(rounding, false, tie,
							widths.second),
						1U << 4, initial, harness);
				}
}

void signed_negative_cross_width()
{
	base::Harness harness;
	for (const auto widths : {std::pair{16U, 32U}, std::pair{16U, 64U},
			std::pair{32U, 64U}, std::pair{64U, 32U}})
		for (const auto rounding : test::roundings) {
			auto initial = base::state(widths.first + widths.second +
				static_cast<unsigned>(rounding));
			test::set_gpr_source(initial, 29U, widths.first,
				test::floating_bits(widths.first, true, false));
			test::require_fpr(test::fpr_instruction(rounding, false,
				widths.first, widths.second, 28U, 29U), widths.second,
				28U, test::expected_rounded(rounding, true, false,
					widths.second), 1U << 4, initial, harness);
		}
}

} // namespace

int main()
{
	every_cross_width_rounding();
	signed_negative_cross_width();
	return 0;
}
