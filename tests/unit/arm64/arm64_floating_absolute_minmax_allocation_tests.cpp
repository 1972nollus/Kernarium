// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_absolute_minmax_test_support.hpp"

namespace test = arm64_floating_absolute_minmax_test;

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

void every_allocated_register_field(test::engine_tests::EngineHarness &harness,
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
					field * 32U + value);
				initial.fpcr = 0U;
				initial.fpsr = test::basic::fpsr_underflow;
				const auto &format =
					test::basic::format_bits(form.width);
				test::horizontal::fill_lanes(initial,
					registers.first,
					form.vector_bytes / (form.width / 8U),
					form.width, format.one | format.sign);
				test::horizontal::fill_lanes(initial,
					registers.second,
					form.vector_bytes / (form.width / 8U),
					form.width, format.one);
				char name[176];
				std::snprintf(name, sizeof(name),
					"absolute minmax allocated form=%u field=%u value=%u",
					form_index, field, value);
				test::check(harness, name, form, registers,
					initial, counts);
			}
		for (const auto registers : test::register_cases) {
			auto initial = test::state(
				5000U + form_index * 17U +
				registers.destination);
			initial.fpcr = 0U;
			initial.fpsr = test::basic::fpsr_inexact;
			const auto &format =
				test::basic::format_bits(form.width);
			test::horizontal::fill_lanes(initial,
				registers.first,
				form.vector_bytes / (form.width / 8U),
				form.width, format.two | format.sign);
			test::horizontal::fill_lanes(initial,
				registers.second,
				form.vector_bytes / (form.width / 8U),
				form.width, format.two);
			test::check(harness,
				"absolute minmax allocated register alias",
				form, registers, initial, counts);
		}
		++form_index;
	});
	test::common::require(form_index == 10U);
}

void reserved_one_double_is_always_tested_by_jit(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	for (const auto operation : {test::Operation::absolute_maximum,
		test::Operation::absolute_minimum})
		for (unsigned field = 0; field < 3U; ++field)
			for (unsigned value = 0; value < 32U; ++value) {
				test::Registers registers{0, 1, 2};
				set_register_field(registers, field, value);
				const test::Form form{operation, 64U, 16U};
				const auto invalid =
					test::instruction(form, registers) &
					~0x4000'0000U;
				harness.check_illegal(
					"absolute minmax reserved .1D",
					invalid,
					test::state(10'000U +
						field * 32U + value));
				counts.add(test::engine_tests::
					NativeRequirements::baseline(), true);
			}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts expected;
	every_allocated_register_field(harness, expected);
	reserved_one_double_is_always_tested_by_jit(harness, expected);
	test::require_counts(harness, expected);
	harness.print_summary("arm64-floating-absolute-minmax-allocation");
}
