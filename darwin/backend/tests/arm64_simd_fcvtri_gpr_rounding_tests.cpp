// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtzi_test_support.hpp"

namespace test = arm64_simd_fcvtzi_test;
namespace base = arm64_simd_test;

namespace {

void every_rounding_signedness_and_width()
{
	base::Harness harness;
	for (const unsigned source_width : {16U, 32U, 64U})
		for (const unsigned integer_width : {32U, 64U})
			for (const auto rounding : test::roundings)
				for (const bool unsigned_result : {false, true})
					for (const bool tie : {false, true}) {
						auto initial = base::state(source_width + integer_width +
							static_cast<unsigned>(rounding) + tie);
						test::set_gpr_source(initial, 29U, source_width,
							test::floating_bits(source_width, false, tie));
						test::require_gpr(test::gpr_instruction(rounding,
							unsigned_result, source_width, integer_width,
							28U, 29U), integer_width, 28U,
							test::expected_rounded(rounding, false, tie,
								integer_width),
							1U << 4, initial, harness);
					}
}

void every_signed_negative_rounding()
{
	base::Harness harness;
	for (const unsigned source_width : {16U, 32U, 64U})
		for (const unsigned integer_width : {32U, 64U})
			for (const auto rounding : test::roundings) {
				auto initial = base::state(source_width + integer_width +
					static_cast<unsigned>(rounding));
				test::set_gpr_source(initial, 29U, source_width,
					test::floating_bits(source_width, true, false));
				test::require_gpr(test::gpr_instruction(rounding, false,
					source_width, integer_width, 28U, 29U), integer_width,
					28U, test::expected_rounded(rounding, true, false,
						integer_width), 1U << 4, initial, harness);
			}
}

} // namespace

int main()
{
	every_rounding_signedness_and_width();
	every_signed_negative_rounding();
	return 0;
}
