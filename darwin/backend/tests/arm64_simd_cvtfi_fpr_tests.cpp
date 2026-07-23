// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_cvtfi_test_support.hpp"

namespace test = arm64_simd_cvtfi_test;
namespace base = arm64_simd_test;

int main()
{
	base::Harness harness;
	for (const auto widths : {std::pair{32U, 16U}, std::pair{64U, 16U},
			std::pair{64U, 32U}, std::pair{32U, 64U}})
		for (const bool unsigned_source : {false, true})
			for (const unsigned destination : {28U, 29U}) {
				auto initial = base::state(widths.first + widths.second +
					destination);
				initial.fpcr = 4U;
				test::set_lane(initial, 29U, widths.first, 0U, 1U);
				const std::array output{
					test::fp::precision::one(widths.second)};
				test::require_vector(test::fpr_instruction(unsigned_source,
					widths.first, widths.second, destination, 29U),
					widths.second, widths.second / 8U, true, destination,
					output, 0U, initial, harness);
			}
	return 0;
}
