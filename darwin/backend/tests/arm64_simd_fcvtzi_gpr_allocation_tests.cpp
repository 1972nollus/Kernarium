// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtzi_test_support.hpp"

namespace test = arm64_simd_fcvtzi_test;
namespace base = arm64_simd_test;

namespace {

void every_source_integer_width_signedness_and_discard()
{
	base::Harness harness;
	for (const unsigned source_width : {16U, 32U, 64U})
		for (const unsigned integer_width : {32U, 64U})
			for (const bool unsigned_result : {false, true})
				for (const unsigned destination : {28U, 31U}) {
					auto initial = base::state(source_width + integer_width +
						unsigned_result + destination);
					test::set_gpr_source(initial, 29U, source_width,
						test::precision::one(source_width));
					test::require_gpr(test::gpr_instruction(unsigned_result,
						source_width, integer_width, destination, 29U),
						integer_width, destination, 1U, 0U, initial, harness);
				}
}

void reserved_source_type_is_early_illegal()
{
	base::Harness harness;
	const auto reserved = test::gpr_instruction(false, 32U, 32U,
		28U, 29U) | 0x0080'0000U;
	harness.require_illegal(reserved, base::state(90U));
}

} // namespace

int main()
{
	every_source_integer_width_signedness_and_discard();
	reserved_source_type_is_early_illegal();
	return 0;
}
