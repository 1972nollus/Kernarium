// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtzi_test_support.hpp"

namespace test = arm64_simd_fcvtzi_test;
namespace base = arm64_simd_test;

namespace {

void exact_scaled_values_and_signed_negatives()
{
	base::Harness harness;
	for (const unsigned source_width : {16U, 32U, 64U})
		for (const unsigned integer_width : {32U, 64U})
			for (const bool unsigned_result : {false, true})
				for (const unsigned fractional_bits : {1U, 8U}) {
					auto initial = base::state(source_width + integer_width +
						fractional_bits + unsigned_result);
					test::set_gpr_source(initial, 29U, source_width,
						test::floating_bits(source_width, false, false));
					const std::uint64_t expected =
						std::uint64_t{3} << (fractional_bits - 1U);
					test::require_gpr(test::gpr_fixed_instruction(
						unsigned_result, source_width, integer_width,
						fractional_bits, 28U, 29U), integer_width, 28U,
						expected, 0U, initial, harness);
				}

	for (const unsigned source_width : {16U, 32U, 64U}) {
		auto initial = base::state(80U + source_width);
		test::set_gpr_source(initial, 29U, source_width,
			test::floating_bits(source_width, true, false));
		test::require_gpr(test::gpr_fixed_instruction(false, source_width,
			64U, 8U, 28U, 29U), 64U, 28U,
			~std::uint64_t{384} + 1U, 0U, initial, harness);
	}
}

} // namespace

int main()
{
	exact_scaled_values_and_signed_negatives();
	return 0;
}
