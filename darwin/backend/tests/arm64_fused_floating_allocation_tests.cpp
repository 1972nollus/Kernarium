// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fused_floating_test_support.hpp"

namespace test = arm64_fused_floating_test;

namespace {

void set_register_field(test::Registers &registers, unsigned field,
	unsigned value)
{
	switch (field) {
	case 0: registers.destination = value; break;
	case 1: registers.left = value; break;
	case 2: registers.right = value; break;
	case 3: registers.addend = value; break;
	default: test::common::require(false);
	}
}

[[nodiscard]] std::uint64_t unit_result_bits(const test::Form &form)
{
	const auto &format = test::basic::format_bits(form.width);
	if (form.subtract)
		return format.positive_zero;
	return form.negate ?
		format.two | format.sign : format.two;
}

void prepare_unit_inputs(uml::backend::arm64::CpuState &initial,
	const test::Form &form, test::Registers registers)
{
	const auto one = test::basic::format_bits(form.width).one;
	for (const unsigned reg : {
		registers.destination, registers.left, registers.right,
		registers.addend,
	})
		test::fill_active_lanes(initial, reg, form, one);
}

void every_allocated_field_value(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &expected)
{
	unsigned form_index{};
	test::for_each_form([&](const test::Form &form) {
		constexpr test::Registers base{0, 1, 2, 3};
		const unsigned fields = form.scalar ? 4U : 3U;
		for (unsigned field = 0; field < fields; ++field)
			for (unsigned value = 0; value < 32U; ++value) {
				auto registers = base;
				set_register_field(registers, field, value);
				auto initial = test::state(
					1000U + form_index * 131U +
					field * 37U + value);
				initial.fpcr = 0;
				initial.fpsr = 0;
				prepare_unit_inputs(initial, form, registers);
				char name[176];
				std::snprintf(name, sizeof(name),
					"fused floating allocated form=%u field=%u value=%u",
					form_index, field, value);
				test::check_uniform(harness, name, form, registers,
					initial, unit_result_bits(form));
				expected.add(test::native_requirements(form));
			}
		for (const auto registers : test::register_cases) {
			auto initial = test::state(
				5000U + form_index * 17U +
				registers.destination);
			initial.fpcr = 0;
			initial.fpsr = 0;
			prepare_unit_inputs(initial, form, registers);
			char name[160];
			std::snprintf(name, sizeof(name),
				"fused floating allocated alias form=%u d=%u n=%u m=%u a=%u",
				form_index, registers.destination, registers.left,
				registers.right, registers.addend);
			test::check_uniform(harness, name, form, registers,
				initial, unit_result_bits(form));
			expected.add(test::native_requirements(form));
		}
		++form_index;
	});
	test::common::require(form_index == 22U);
}

void reserved_scalar_type_is_rejected(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &expected)
{
	for (const bool negate : {false, true})
		for (const bool subtract : {false, true}) {
			const test::Form form{
				true, negate, subtract, 32U, 16U};
			for (unsigned field = 0; field < 4U; ++field)
				for (unsigned value = 0; value < 32U; ++value) {
					test::Registers registers{0, 1, 2, 3};
					set_register_field(registers, field, value);
					auto encoded =
						test::instruction(form, registers);
					encoded &= ~(3U << 22);
					encoded |= 2U << 22;
					char name[160];
					std::snprintf(name, sizeof(name),
						"fused floating reserved scalar type negate=%u subtract=%u field=%u value=%u",
						negate, subtract, field, value);
					harness.check_illegal(name, encoded,
						test::state(9000U + field * 32U + value));
					expected.add(
						test::engine_tests::NativeRequirements::
							baseline(),
						true);
				}
		}
}

void reserved_vector_shape_is_rejected(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &expected)
{
	for (const bool subtract : {false, true}) {
		const test::Form form{
			false, false, subtract, 64U, 16U};
		for (unsigned field = 0; field < 3U; ++field)
			for (unsigned value = 0; value < 32U; ++value) {
				test::Registers registers{0, 1, 2, 0};
				set_register_field(registers, field, value);
				auto encoded = test::instruction(form, registers);
				encoded &= ~0x4000'0000U;
				char name[160];
				std::snprintf(name, sizeof(name),
					"fused floating reserved vector 1d subtract=%u field=%u value=%u",
					subtract, field, value);
				harness.check_illegal(name, encoded,
					test::state(10'000U + field * 32U + value));
				expected.add(
					test::engine_tests::NativeRequirements::
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
	every_allocated_field_value(harness, expected);
	reserved_scalar_type_is_rejected(harness, expected);
	reserved_vector_shape_is_rejected(harness, expected);
	test::require_counts(harness, expected);
	harness.print_summary("arm64-fused-floating-allocation");
}
