// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_horizontal_test_support.hpp"

namespace test = arm64_floating_horizontal_test;

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

[[nodiscard]] unsigned register_fields(const test::Form &form)
{
	return form.kind == test::Kind::scalar_pairwise ||
		form.kind == test::Kind::reduction ? 2U : 3U;
}

void prepare_unit_inputs(test::arm64::CpuState &initial,
	const test::Form &form, test::Registers registers)
{
	const auto &format = test::basic::format_bits(form.width);
	test::fill_lanes(initial, registers.first, test::source_lanes(form),
		form.width, format.one);
	if (form.kind == test::Kind::scalar_binary ||
	    form.kind == test::Kind::vector_binary ||
	    form.kind == test::Kind::vector_pairwise)
		test::fill_lanes(initial, registers.second,
			test::source_lanes(form), form.width, format.one);
}

[[nodiscard]] std::uint64_t unit_result(const test::Form &form)
{
	const auto &format = test::basic::format_bits(form.width);
	if (form.operation == test::Operation::absolute_difference)
		return format.positive_zero;
	if (form.operation == test::Operation::add)
		return format.two;
	return format.one;
}

void every_allocated_register_field(test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned form_index{};
	test::for_each_form([&](const test::Form &form) {
		for (unsigned field = 0; field < register_fields(form); ++field)
			for (unsigned value = 0; value < 32U; ++value) {
				test::Registers registers{0, 1, 2};
				set_register_field(registers, field, value);
				auto initial = test::state(
					1000U + form_index * 101U +
					field * 32U + value);
				initial.fpcr = 0U;
				initial.fpsr = test::basic::fpsr_inexact;
				prepare_unit_inputs(initial, form, registers);
				char name[176];
				std::snprintf(name, sizeof(name),
					"horizontal allocated form=%u field=%u value=%u",
					form_index, field, value);
				test::check_uniform(harness, name, form, registers,
					initial, unit_result(form), counts);
			}
		for (const auto registers : test::register_cases) {
			auto initial = test::state(
				20'000U + form_index * 17U +
				registers.destination);
			initial.fpcr = 0U;
			initial.fpsr = test::basic::fpsr_underflow;
			prepare_unit_inputs(initial, form, registers);
			char name[192];
			std::snprintf(name, sizeof(name),
				"horizontal allocated alias form=%u d=%u n=%u m=%u",
				form_index, registers.destination, registers.first,
				registers.second);
			test::check_uniform(harness, name, form, registers,
				initial, unit_result(form), counts);
		}
		++form_index;
	});
	test::common::require(form_index == 92U);
}

void check_reserved(test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts, std::string_view label,
	std::uint32_t instruction, unsigned seed)
{
	harness.check_illegal(label, instruction, test::state(seed));
	counts.add(test::engine_tests::NativeRequirements::baseline(), true);
}

void reserved_scalar_types(test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	for (const auto operation : test::minmax_operations) {
		const test::Form form{
			operation, test::Kind::scalar_binary, 32U, 16U};
		for (unsigned field = 0; field < 3U; ++field)
			for (unsigned value = 0; value < 32U; ++value) {
				test::Registers registers{0, 1, 2};
				set_register_field(registers, field, value);
				auto encoded = test::instruction(form, registers);
				encoded &= ~(3U << 22);
				encoded |= 2U << 22;
				check_reserved(harness, counts,
					"horizontal reserved scalar ftype=10",
					encoded, 30'000U + field * 32U + value);
			}
	}
}

void reserved_scalar_pairwise_half_size(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	for (const auto operation : {test::Operation::add,
		test::Operation::maximum, test::Operation::maximum_number,
		test::Operation::minimum, test::Operation::minimum_number}) {
		const test::Form form{
			operation, test::Kind::scalar_pairwise, 16U, 16U};
		for (unsigned field = 0; field < 2U; ++field)
			for (unsigned value = 0; value < 32U; ++value) {
				test::Registers registers{0, 1, 2};
				set_register_field(registers, field, value);
				check_reserved(harness, counts,
					"horizontal reserved scalar pairwise half sz=1",
					test::instruction(form, registers) |
						0x0040'0000U,
					40'000U + field * 32U + value);
			}
	}
}

void reserved_vector_one_double(test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	std::array<test::Form, 10> forms{};
	unsigned count{};
	forms[count++] = {test::Operation::absolute_difference,
		test::Kind::vector_binary, 64U, 16U};
	forms[count++] = {test::Operation::add,
		test::Kind::vector_pairwise, 64U, 16U};
	for (const auto operation : test::minmax_operations) {
		forms[count++] = {operation, test::Kind::vector_binary, 64U, 16U};
		forms[count++] = {operation, test::Kind::vector_pairwise, 64U, 16U};
	}
	test::common::require(count == forms.size());
	for (unsigned form_index = 0; form_index < forms.size(); ++form_index)
		for (unsigned field = 0; field < 3U; ++field)
			for (unsigned value = 0; value < 32U; ++value) {
				test::Registers registers{0, 1, 2};
				set_register_field(registers, field, value);
				check_reserved(harness, counts,
					"horizontal reserved vector .1D",
					test::instruction(forms[form_index], registers) &
						~0x4000'0000U,
					50'000U + form_index * 97U +
						field * 32U + value);
			}
}

void reserved_reduction_shapes(test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	for (const auto operation : test::minmax_operations) {
		const test::Form form{
			operation, test::Kind::reduction, 32U, 16U};
		for (unsigned shape = 0; shape < 3U; ++shape)
			for (unsigned field = 0; field < 2U; ++field)
				for (unsigned value = 0; value < 32U; ++value) {
					test::Registers registers{0, 1, 2};
					set_register_field(registers, field, value);
					auto encoded =
						test::instruction(form, registers);
					if (shape != 1U)
						encoded &= ~0x4000'0000U;
					if (shape != 0U)
						encoded |= 0x0040'0000U;
					check_reserved(harness, counts,
						"horizontal reserved reduction shape",
						encoded, 70'000U + shape * 101U +
							field * 32U + value);
				}
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts expected;
	every_allocated_register_field(harness, expected);
	reserved_scalar_types(harness, expected);
	reserved_scalar_pairwise_half_size(harness, expected);
	reserved_vector_one_double(harness, expected);
	reserved_reduction_shapes(harness, expected);
	test::require_counts(harness, expected);
	harness.print_summary("arm64-floating-horizontal-allocation");
}
