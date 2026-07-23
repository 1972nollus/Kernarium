// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcmp_test_support.hpp"

namespace test = arm64_simd_fcmp_test;
namespace base = arm64_simd_test;

namespace {

void every_direct_encoding()
{
	base::Harness harness;
	for (const unsigned bytes : {2U, 4U, 8U})
		for (const bool signaling : {false, true})
			for (const bool zero : {false, true}) {
				auto initial = base::state(bytes + signaling + zero);
				initial.fpsr = 0x0800'0010U;
				test::put(initial, 29U, bytes, test::positive_one(bytes));
				test::put(initial, 30U, bytes, test::positive_two(bytes));
				test::require_result(test::direct_instruction(bytes,
					signaling, zero, 29U, zero ? 0U : 30U),
					zero ? 0x2000'0000U : 0x8000'0000U, 0U,
					initial, harness);
			}
}

void every_conditional_encoding()
{
	base::Harness harness;
	for (const unsigned bytes : {2U, 4U, 8U})
		for (const bool signaling : {false, true})
			for (unsigned condition = 0; condition < 16U; ++condition) {
				auto initial = base::state(bytes * 17U + condition + signaling);
				initial.nzcv = ((condition * 7U) & 15U) << 28;
				initial.fpsr = 0x0800'0010U;
				test::put(initial, 29U, bytes, test::positive_one(bytes));
				test::put(initial, 30U, bytes, test::positive_two(bytes));
				const unsigned fallback = (condition * 5U + 3U) & 15U;
				const bool passed = uml::backend::arm64::detail::condition_holds(
					initial.nzcv, condition);
				test::require_result(test::conditional_instruction(bytes,
					signaling, condition, fallback, 29U, 30U),
					passed ? 0x8000'0000U : fallback << 28, 0U,
					initial, harness);
			}
}

void reserved_encodings_are_early_illegal()
{
	base::Harness harness;
	for (const bool signaling : {false, true}) {
		for (const bool zero : {false, true}) {
			const auto type_reserved = 0x1e20'2000U | (2U << 22) |
				(29U << 5) | (signaling ? 16U : 0U) |
				(zero ? 8U : 0U);
			harness.require_illegal(type_reserved, base::state(40U));
		}
		const auto conditional_reserved = 0x1e20'0400U | (2U << 22) |
			(30U << 16) | (9U << 12) | (29U << 5) |
			(signaling ? 16U : 0U) | 7U;
		harness.require_illegal(conditional_reserved, base::state(41U));
	}
	for (unsigned bit = 0; bit < 3U; ++bit)
		harness.require_illegal(
			test::direct_instruction(4U, false, false, 29U, 30U) |
				(1U << bit),
			base::state(42U + bit));
	harness.require_illegal(
		test::direct_instruction(8U, true, true, 29U, 30U),
		base::state(46U));
}

} // namespace

int main()
{
	every_direct_encoding();
	every_conditional_encoding();
	reserved_encodings_are_early_illegal();
	return 0;
}
