// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtzi_test_support.hpp"

namespace test = arm64_simd_fcvtzi_test;
namespace base = arm64_simd_test;

namespace {

void every_width_signedness_scale_and_discard()
{
	base::Harness harness;
	for (const unsigned source_width : {16U, 32U, 64U})
		for (const unsigned integer_width : {32U, 64U})
			for (const bool unsigned_result : {false, true})
				for (unsigned fractional_bits = 1U;
				     fractional_bits <= integer_width; ++fractional_bits)
					for (const unsigned destination : {28U, 31U}) {
						auto initial = base::state(source_width + integer_width +
							fractional_bits + destination);
						test::set_gpr_source(initial, 29U, source_width, 0U);
						test::require_gpr(test::gpr_fixed_instruction(
							unsigned_result, source_width, integer_width,
							fractional_bits, destination, 29U),
							integer_width, destination, 0U, 0U,
							initial, harness);
					}
}

} // namespace

int main()
{
	every_width_signedness_scale_and_discard();
	return 0;
}
