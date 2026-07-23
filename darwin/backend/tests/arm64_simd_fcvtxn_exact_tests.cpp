// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtxn_test_support.hpp"

namespace test = arm64_simd_fcvtxn_test;
namespace base = arm64_simd_test;

namespace {

void exact_and_inexact_results()
{
	base::Harness harness;
	for (const auto form : {test::Form::scalar, test::Form::vector_lower,
	     test::Form::vector_upper}) {
		auto exact = base::state(static_cast<unsigned>(form));
		const unsigned count = test::lanes(form);
		std::array<std::uint32_t, 2> exact_outputs{};
		for (unsigned lane = 0; lane < count; ++lane) {
			test::set_source_lane(exact, 29U, lane,
				lane == 0U ? 0x3ff0'0000'0000'0000ULL :
					0xc000'0000'0000'0000ULL);
			exact_outputs[lane] = lane == 0U ? 0x3f80'0000U : 0xc000'0000U;
		}
		test::require_result(test::instruction(form, 28U, 29U), form, 28U,
			std::span{exact_outputs}.first(count), 0U, exact, harness);

		auto inexact = base::state(10U + static_cast<unsigned>(form));
		std::array<std::uint32_t, 2> odd_outputs{};
		for (unsigned lane = 0; lane < count; ++lane) {
			test::set_source_lane(inexact, 29U, lane,
				lane == 0U ? 0x3ff0'0000'1000'0000ULL :
					0xbff0'0000'1000'0000ULL);
			odd_outputs[lane] = lane == 0U ? 0x3f80'0001U : 0xbf80'0001U;
		}
		test::require_result(test::instruction(form, 28U, 29U), form, 28U,
			std::span{odd_outputs}.first(count), 1U << 4, inexact, harness);
	}
}

} // namespace

int main()
{
	exact_and_inexact_results();
	return 0;
}
