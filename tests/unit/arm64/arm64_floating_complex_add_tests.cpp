// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_complex_test_support.hpp"

namespace test = arm64_floating_complex_test;

namespace {

template<typename Floating>
void check_form(test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts, const test::Form &form,
	unsigned form_index)
{
	constexpr test::Registers registers{0, 1, 2};
	auto initial = test::state(100U + form_index);
	initial.fpcr = 0;
	initial.fpsr = 1U << 27;
	test::prepare_exact<Floating>(initial, form, registers);
	char name[160];
	std::snprintf(name, sizeof(name),
		"FPSIMD-0057 complex add exact form=%u width=%u bytes=%u rotation=%u",
		form_index, form.width, form.vector_bytes, form.rotation);
	test::check(harness, counts, name, form, registers, initial,
		test::expected_exact<Floating>(initial, form, registers));
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts expected;
	unsigned form_index{};
	test::for_each_form([&](const test::Form &form) {
		if (form.operation != test::Operation::add)
			return;
		if (form.width == 16U)
			check_form<_Float16>(
				harness, expected, form, form_index);
		else if (form.width == 32U)
			check_form<float>(
				harness, expected, form, form_index);
		else
			check_form<double>(
				harness, expected, form, form_index);
		++form_index;
	});
	test::common::require(form_index == 10U);
	test::require_counts(harness, expected);
	harness.print_summary("arm64-floating-complex-add");
}
