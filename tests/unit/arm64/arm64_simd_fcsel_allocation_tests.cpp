// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcsel_test_support.hpp"

namespace test = arm64_simd_fcsel_test;

namespace {

void every_precision_condition_and_register_field()
{
	test::engine_tests::EngineHarness harness;
	for (const unsigned bytes : {2U, 4U, 8U})
		for (unsigned condition = 0; condition < 16U; ++condition) {
			for (unsigned field = 0; field < 32U; ++field) {
				const unsigned destination = field;
				const unsigned first = 29U;
				const unsigned second = 30U;
				auto initial = test::state(
					bytes * 37U + condition * 11U + field);
				initial.nzcv = (condition ^ 9U) << 28;
				harness.check_one("FCSEL destination-field allocation",
					test::instruction(bytes, condition, destination,
						first, second),
					initial,
					test::expected_state(initial, bytes, condition,
						destination, first, second),
					test::native_requirements(bytes));
			}
			for (unsigned field = 0; field < 32U; ++field) {
				const unsigned destination = 28U;
				const unsigned first = field;
				const unsigned second = 30U;
				auto initial = test::state(
					bytes * 41U + condition * 13U + field);
				initial.nzcv = (condition ^ 6U) << 28;
				harness.check_one("FCSEL first-source-field allocation",
					test::instruction(bytes, condition, destination,
						first, second),
					initial,
					test::expected_state(initial, bytes, condition,
						destination, first, second),
					test::native_requirements(bytes));
			}
			for (unsigned field = 0; field < 32U; ++field) {
				const unsigned destination = 28U;
				const unsigned first = 29U;
				const unsigned second = field;
				auto initial = test::state(
					bytes * 43U + condition * 17U + field);
				initial.nzcv = (condition ^ 3U) << 28;
				harness.check_one("FCSEL second-source-field allocation",
					test::instruction(bytes, condition, destination,
						first, second),
					initial,
					test::expected_state(initial, bytes, condition,
						destination, first, second),
					test::native_requirements(bytes));
			}
		}
	harness.print_summary("simd-fcsel-allocation-valid");
}

void every_reserved_type_condition_and_register_field()
{
	test::engine_tests::EngineHarness harness;
	for (unsigned condition = 0; condition < 16U; ++condition)
		for (unsigned field = 0; field < 32U; ++field) {
			const auto initial =
				test::state(47U + condition * 19U + field);
			harness.check_illegal("FCSEL reserved type",
				test::raw_instruction(2U, condition, field,
					(field + 1U) & 31U, (field + 2U) & 31U),
				initial,
				test::engine_tests::NativeRequirements::features(
					::uml::cpu::ArmFeature::floating_point));
		}
	harness.print_summary("simd-fcsel-allocation-reserved");
}

} // namespace

int main()
{
	every_precision_condition_and_register_field();
	every_reserved_type_condition_and_register_field();
	return 0;
}
