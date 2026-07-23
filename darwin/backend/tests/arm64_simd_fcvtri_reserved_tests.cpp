// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvtzi_test_support.hpp"

namespace test = arm64_simd_fcvtzi_test;
namespace base = arm64_simd_test;

namespace {

void reserved_widths_and_opcode_pairs_are_early_illegal()
{
	base::Harness harness;
	for (const auto rounding : test::roundings)
		for (const bool unsigned_result : {false, true}) {
			harness.require_illegal(test::gpr_instruction(rounding,
				unsigned_result, 32U, 32U, 28U, 29U) | 0x0080'0000U,
				base::state(90U));
			harness.require_illegal(test::fpr_instruction(rounding,
				unsigned_result, 32U, 64U, 28U, 29U) | 0x0080'0000U,
				base::state(91U));
			harness.require_illegal(test::fpr_instruction(rounding,
				unsigned_result, 32U, 32U, 28U, 29U), base::state(92U));
			harness.require_illegal(test::fpr_instruction(rounding,
				unsigned_result, 64U, 64U, 28U, 29U), base::state(93U));
			const test::Shape short_double{64U, 8U, false, false};
			harness.require_illegal(test::simd_instruction(rounding,
				unsigned_result, short_double, 28U, 29U), base::state(94U));
		}

	for (const unsigned opcode : {4U, 5U, 6U, 7U}) {
		const auto reserved = 0x1e20'0000U | (1U << 19) |
			(opcode << 16) | (29U << 5) | 28U;
		harness.require_illegal(reserved, base::state(95U + opcode));
	}
}

} // namespace

int main()
{
	reserved_widths_and_opcode_pairs_are_early_illegal();
	return 0;
}
