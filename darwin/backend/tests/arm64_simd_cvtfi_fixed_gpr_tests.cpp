// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_cvtfi_test_support.hpp"

namespace test = arm64_simd_cvtfi_test;
namespace base = arm64_simd_test;

int main()
{
	base::Harness harness;
	for (const unsigned integer_width : {32U, 64U})
		for (const unsigned destination_width : {16U, 32U, 64U})
			for (const bool unsigned_source : {false, true})
				for (unsigned fractional_bits = 1U;
				     fractional_bits <= integer_width; ++fractional_bits)
					for (const unsigned source : {29U, 31U}) {
						auto initial = base::state(integer_width +
							destination_width + fractional_bits + source);
						initial.registers[29] = 0U;
						const std::array output{std::uint64_t{0}};
						test::require_vector(test::gpr_fixed_instruction(
							unsigned_source, integer_width, destination_width,
							fractional_bits, 28U, source),
							destination_width, destination_width / 8U, true,
							28U, output, 0U, initial, harness);
					}
	return 0;
}
