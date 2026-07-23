// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcmp_test_support.hpp"

namespace test = arm64_simd_fcmp_test;
namespace base = arm64_simd_test;

namespace {

void every_condition_and_nzcv_input()
{
	base::Harness harness;
	for (const unsigned bytes : {2U, 4U, 8U})
		for (const bool signaling : {false, true})
			for (unsigned condition = 0; condition < 16U; ++condition)
				for (unsigned flags = 0; flags < 16U; ++flags) {
					auto initial = base::state(
						bytes * 31U + condition + flags + signaling);
					initial.nzcv = flags << 28;
					initial.fpsr = 0x0800'0010U;
					test::put(initial, 29U, bytes, test::negative_one(bytes));
					test::put(initial, 30U, bytes, test::positive_one(bytes));
					const unsigned fallback =
						(condition * 11U + flags * 3U + 1U) & 15U;
					const bool passed =
						uml::backend::arm64::detail::condition_holds(
							initial.nzcv, condition);
					test::require_result(test::conditional_instruction(bytes,
						signaling, condition, fallback, 29U, 30U),
						passed ? 0x8000'0000U : fallback << 28,
						0U, initial, harness);
				}
}

void failed_condition_does_not_observe_operands()
{
	base::Harness harness;
	for (const unsigned bytes : {2U, 4U, 8U})
		for (const bool signaling : {false, true}) {
			auto initial = base::state(bytes + signaling);
			initial.nzcv = 0U; /* EQ fails. */
			initial.fpcr = (1U << 24) | 2U;
			initial.fpsr = 0x10U;
			test::put(initial, 29U, bytes, test::signaling_nan(bytes));
			test::put(initial, 30U, bytes,
				test::minimum_subnormal(bytes));
			test::require_result(test::conditional_instruction(bytes,
				signaling, 0U, 13U, 29U, 30U), 0xd000'0000U,
				0U, initial, harness);
		}
}

void passed_condition_uses_quiet_or_signaling_compare()
{
	base::Harness harness;
	for (const unsigned bytes : {2U, 4U, 8U})
		for (const bool signaling : {false, true}) {
			auto initial = base::state(bytes * 7U + signaling);
			initial.nzcv = 0x4000'0000U; /* EQ passes. */
			initial.fpsr = 0x0800'0090U;
			test::put(initial, 29U, bytes, test::quiet_nan(bytes));
			test::put(initial, 30U, bytes, test::positive_one(bytes));
			test::require_result(test::conditional_instruction(bytes,
				signaling, 0U, 9U, 29U, 30U), 0x3000'0000U,
				signaling ? 1U : 0U, initial, harness);
		}
}

} // namespace

int main()
{
	every_condition_and_nzcv_input();
	failed_condition_does_not_observe_operands();
	passed_condition_uses_quiet_or_signaling_compare();
	return 0;
}
