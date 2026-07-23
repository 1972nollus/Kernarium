// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcmp_test_support.hpp"

#include <array>

namespace test = arm64_simd_fcmp_test;
namespace base = arm64_simd_test;

namespace {

struct OrderedCase final {
	std::uint64_t left;
	std::uint64_t right;
	std::uint32_t nzcv;
};

void ordered_register_comparisons()
{
	base::Harness harness;
	for (const unsigned bytes : {2U, 4U, 8U}) {
		const std::array cases{
			OrderedCase{test::positive_zero(bytes), test::negative_zero(bytes),
				0x6000'0000U},
			OrderedCase{test::negative_zero(bytes), test::positive_zero(bytes),
				0x6000'0000U},
			OrderedCase{test::positive_one(bytes), test::positive_one(bytes),
				0x6000'0000U},
			OrderedCase{test::positive_one(bytes), test::positive_two(bytes),
				0x8000'0000U},
			OrderedCase{test::positive_two(bytes), test::positive_one(bytes),
				0x2000'0000U},
			OrderedCase{test::negative_two(bytes), test::negative_one(bytes),
				0x8000'0000U},
			OrderedCase{test::negative_one(bytes), test::negative_two(bytes),
				0x2000'0000U},
			OrderedCase{test::negative_infinity(bytes), test::negative_two(bytes),
				0x8000'0000U},
			OrderedCase{test::positive_infinity(bytes), test::positive_two(bytes),
				0x2000'0000U},
		};
		for (const bool signaling : {false, true})
			for (unsigned index = 0; index < cases.size(); ++index) {
				auto initial = base::state(bytes * 23U + index + signaling);
				initial.fpsr = 0x0800'0090U;
				test::put(initial, 29U, bytes, cases[index].left);
				test::put(initial, 30U, bytes, cases[index].right);
				test::require_result(test::direct_instruction(bytes,
					signaling, false, 29U, 30U), cases[index].nzcv,
					0U, initial, harness);
			}
	}
}

void compare_with_zero_variants()
{
	base::Harness harness;
	for (const unsigned bytes : {2U, 4U, 8U})
		for (const bool signaling : {false, true}) {
			const std::array cases{
				OrderedCase{test::negative_one(bytes), 0U, 0x8000'0000U},
				OrderedCase{test::negative_zero(bytes), 0U, 0x6000'0000U},
				OrderedCase{test::positive_one(bytes), 0U, 0x2000'0000U},
			};
			for (unsigned index = 0; index < cases.size(); ++index) {
				auto initial = base::state(bytes + index + signaling);
				initial.fpsr = 0x10U;
				test::put(initial, 29U, bytes, cases[index].left);
				test::require_result(test::direct_instruction(bytes,
					signaling, true, 29U), cases[index].nzcv, 0U,
					initial, harness);
			}
		}
}

void nan_signaling_and_exception_accumulation()
{
	base::Harness harness;
	for (const unsigned bytes : {2U, 4U, 8U})
		for (const bool zero : {false, true})
			for (const bool quiet_source : {false, true})
				for (const bool signaling_compare : {false, true}) {
					auto initial = base::state(bytes * 13U + zero + quiet_source);
					initial.fpsr = 0x0800'0090U;
					const auto nan = quiet_source ? test::quiet_nan(bytes) :
						test::signaling_nan(bytes);
					test::put(initial, 29U, bytes, nan);
					test::put(initial, 30U, bytes, test::positive_one(bytes));
					const std::uint32_t expected_ioc =
						(!quiet_source || signaling_compare) ? 1U : 0U;
					test::require_result(test::direct_instruction(bytes,
						signaling_compare, zero, 29U, zero ? 0U : 30U),
						0x3000'0000U, expected_ioc, initial, harness);
				}

	for (const unsigned bytes : {2U, 4U, 8U}) {
		auto initial = base::state(bytes * 19U);
		initial.fpsr = 0x80U;
		test::put(initial, 29U, bytes, test::positive_one(bytes));
		test::put(initial, 30U, bytes, test::signaling_nan(bytes));
		test::require_result(test::direct_instruction(bytes, false, false,
			29U, 30U), 0x3000'0000U, 1U, initial, harness);
	}
}

} // namespace

int main()
{
	ordered_register_comparisons();
	compare_with_zero_variants();
	nan_signaling_and_exception_accumulation();
	return 0;
}
