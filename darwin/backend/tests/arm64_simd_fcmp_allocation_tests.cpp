// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcmp_test_support.hpp"

namespace test = arm64_simd_fcmp_test;

namespace {

void every_direct_allocation()
{
	test::engine_tests::EngineHarness harness;
	for (unsigned type = 0; type < 4U; ++type)
		for (unsigned low = 0; low < 32U; ++low)
			for (unsigned field = 0; field < 32U; ++field) {
				const unsigned left = field;
				const unsigned right = (low & 8U) != 0U ? 0U :
					(field * 13U + low * 7U + type) & 31U;
				const auto instruction =
					test::direct_raw_instruction(
						type, right, left, low);
				auto initial = test::state(
					type * 131U + right * 17U +
					left * 5U + low);
				if (!test::direct_valid(type, right, low)) {
					harness.check_illegal(
						"FCMP/FCMPE reserved allocation",
						instruction, initial);
					continue;
				}
				const auto &format = test::format_for_type(type);
				test::put(initial, left, format,
					format.positive_one);
				if ((low & 8U) == 0U)
					test::put(initial, right, format,
						left == right ?
							format.positive_one :
							format.positive_two);
				test::check_direct(harness,
					"FCMP/FCMPE valid allocation",
					format, (low & 16U) != 0U,
					(low & 8U) != 0U, left, right,
					initial);
			}

	/*
	 * The zero form reserves every nonzero Rm value. Cover that complete
	 * range separately because valid zero encodings above necessarily fix
	 * Rm to zero.
	 */
	for (unsigned type = 0; type < 4U; ++type)
		for (const unsigned low : {8U, 24U})
			for (unsigned right = 1U; right < 32U; ++right) {
				const unsigned left =
					(right * 11U + low + type) & 31U;
				const auto instruction =
					test::direct_raw_instruction(
						type, right, left, low);
				auto initial = test::state(
					type * 131U + right * 17U +
					left * 5U + low);
				harness.check_illegal(
					"FCMP/FCMPE reserved zero Rm",
					instruction, initial);
			}
	harness.print_summary("arm64-simd-fcmp-allocation-direct");
}

void every_conditional_allocation()
{
	test::engine_tests::EngineHarness harness;
	for (unsigned type = 0; type < 4U; ++type)
		for (unsigned condition = 0; condition < 16U; ++condition)
			for (unsigned low = 0; low < 32U; ++low)
				for (unsigned field = 0; field < 32U; ++field) {
					const unsigned left = field;
					const unsigned right =
						(field * 13U + condition * 7U +
						 low * 3U + type) & 31U;
					const auto instruction =
						test::conditional_raw_instruction(
							type, right, condition,
							left, low);
					auto initial = test::state(
						type * 137U + right * 19U +
						condition * 11U + left * 7U +
						low);
					initial.nzcv =
						((condition * 5U + low) & 15U)
						<< 28;
					if (!test::conditional_valid(type)) {
						harness.check_illegal(
							"FCCMP/FCCMPE reserved allocation",
							instruction, initial);
						continue;
					}
					const auto &format =
						test::format_for_type(type);
					test::put(initial, left, format,
						format.positive_one);
					test::put(initial, right, format,
						left == right ?
							format.positive_one :
							format.positive_two);
					test::check_conditional(harness,
						"FCCMP/FCCMPE valid allocation",
						format, (low & 16U) != 0U,
						condition, low & 15U, left,
						right, initial);
				}
	harness.print_summary("arm64-simd-fcmp-allocation-conditional");
}

} // namespace

int main()
{
	every_direct_allocation();
	every_conditional_allocation();
	return 0;
}
