// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_complex_test_support.hpp"

namespace test = arm64_floating_complex_test;

namespace {

void set_register_field(test::Registers &registers, unsigned field,
	unsigned value)
{
	switch (field) {
	case 0: registers.destination = value; break;
	case 1: registers.first = value; break;
	case 2: registers.second = value; break;
	default: test::common::require(false);
	}
}

void every_valid_form_and_register_field(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned form_index{};
	test::for_each_form([&](const test::Form &form) {
		for (unsigned field = 0; field < 3U; ++field)
			for (unsigned value = 0; value < 32U; ++value) {
				test::Registers registers{0, 1, 2};
				set_register_field(registers, field, value);
				auto initial = test::state(
					1000U + form_index * 101U +
					field * 37U + value);
				initial.fpcr = 0;
				initial.fpsr = 1U << 27;
				test::prepare_unit(initial, form, registers);
				char name[192];
				std::snprintf(name, sizeof(name),
					"complex allocated form=%u field=%u value=%u",
					form_index, field, value);
				test::check(harness, counts, name, form, registers,
					initial,
					test::expected_unit(initial, form,
						registers));
			}
		for (const auto registers : test::register_cases) {
			auto initial = test::state(
				9000U + form_index * 17U +
				registers.destination);
			initial.fpcr = 0;
			initial.fpsr = 1U << 27;
			test::prepare_unit(initial, form, registers);
			char name[192];
			std::snprintf(name, sizeof(name),
				"complex allocated alias form=%u d=%u n=%u m=%u",
				form_index, registers.destination,
				registers.first, registers.second);
			test::check(harness, counts, name, form, registers,
				initial,
				test::expected_unit(initial, form, registers));
		}
		++form_index;
	});
	test::common::require(form_index == 62U);
}

void every_reserved_vector_control_cell(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed = 20'000U;
	for (const auto operation : {
		test::Operation::add,
		test::Operation::multiply_accumulate,
	}) {
		const unsigned rotations =
			operation == test::Operation::add ? 2U : 4U;
		for (const bool q : {false, true})
			for (unsigned size = 0; size < 4U; ++size)
				for (unsigned rotation = 0;
				     rotation < rotations; ++rotation) {
					if (test::valid_vector_control(q, size))
						continue;
					for (unsigned field = 0; field < 3U; ++field)
						for (unsigned value = 0;
						     value < 32U; ++value) {
							test::Registers registers{
								0, 1, 2};
							set_register_field(
								registers,
								field, value);
							const auto encoded =
								test::
								raw_vector_instruction(
									operation,
									q, size,
									rotation,
									registers);
							char name[224];
							std::snprintf(name,
								sizeof(name),
								"complex reserved vector op=%u q=%u size=%u rot=%u field=%u value=%u",
								static_cast<unsigned>(
									operation),
								q, size,
								rotation,
								field, value);
							harness.check_illegal(
								name, encoded,
								test::state(
									seed++));
							counts.add(
								test::
								engine_tests::
								NativeRequirements::
									baseline(),
								true);
						}
					}
	}
}

void every_reserved_element_control_cell(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed = 40'000U;
	for (const bool q : {false, true})
		for (unsigned size = 0; size < 4U; ++size)
			for (const bool l : {false, true})
				for (const bool h : {false, true})
					for (unsigned rotation = 0;
					     rotation < 4U; ++rotation) {
						if (test::valid_element_control(
							    q, size, l, h))
							continue;
						for (unsigned field = 0;
						     field < 3U; ++field)
							for (unsigned value = 0;
							     value < 32U;
							     ++value) {
								test::Registers
									registers{
										0,
										1,
										2};
								set_register_field(
									registers,
									field,
									value);
								const auto encoded =
									test::
									raw_element_instruction(
										q,
										size,
										l,
										h,
										rotation,
										registers);
								char name[240];
								std::snprintf(
									name,
									sizeof(name),
									"complex reserved element q=%u size=%u l=%u h=%u rot=%u field=%u value=%u",
									q, size,
									l, h,
									rotation,
									field,
									value);
								harness.check_illegal(
									name,
									encoded,
									test::state(
										seed++));
								counts.add(
									test::
									engine_tests::
									NativeRequirements::
										baseline(),
									true);
							}
					}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts expected;
	every_valid_form_and_register_field(harness, expected);
	every_reserved_vector_control_cell(harness, expected);
	every_reserved_element_control_cell(harness, expected);
	test::require_counts(harness, expected);
	harness.print_summary("arm64-floating-complex-allocation");
}
