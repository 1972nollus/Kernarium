// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_multiply_matrix_engine_test_support.hpp"

#include <cstdio>

namespace test = arm64_fp8_multiply_matrix_engine_test;

namespace {

template<typename Function>
void for_each_form(Function &&function)
{
	test::for_each_multiply_form(function);
	test::for_each_matrix_form(function);
}

void every_register_field_and_alias()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed{};
	for_each_form([&](const test::Form &form) {
		for (unsigned role = 0U; role < 3U; ++role) {
			const unsigned count =
				role == 2U && form.by_element ? 8U : 32U;
			for (unsigned reg = 0U; reg < count; ++reg) {
				test::Registers registers{3U, 11U,
					form.by_element ? 7U : 17U};
				if (role == 0U)
					registers.destination = reg;
				else if (role == 1U)
					registers.first = reg;
				else
					registers.second = reg;
				auto initial = test::state(++seed);
				initial.fpmr = test::fpmr(0U, 1U, 3U);
				char name[224];
				std::snprintf(name, sizeof(name),
					"fp8-multiply-matrix-allocation family=%u "
					"width=%u indexed=%u selection=%u index=%u "
					"role=%u reg=%u",
					static_cast<unsigned>(form.family),
					form.width,
					static_cast<unsigned>(form.by_element),
					form.selection, form.index, role, reg);
				test::check(harness, name, form, registers,
					initial);
			}
		}
		for (const auto registers : test::register_aliases) {
			if (form.by_element && registers.second >= 8U)
				continue;
			auto initial = test::state(++seed);
			initial.fpmr = test::fpmr(1U, 0U, 7U);
			test::check(harness,
				"fp8-multiply-matrix-allocation-alias",
				form, registers, initial);
		}
	});
	harness.print_summary("arm64-fp8-multiply-matrix-allocation");
}

} // namespace

int main()
{
	using test::Family;
	static_assert(test::instruction(
		{Family::multiply, 16U, false, 0U, 0U}) ==
		0x0ec2'fc20U);
	static_assert(test::instruction(
		{Family::multiply, 16U, true, 1U, 15U},
		{3U, 5U, 7U}) == 0x4fff'08a3U);
	static_assert(test::instruction(
		{Family::multiply, 32U, false, 3U, 0U}) ==
		0x4e42'c420U);
	static_assert(test::instruction(
		{Family::multiply, 32U, true, 3U, 15U},
		{3U, 5U, 7U}) == 0x6f7f'88a3U);
	static_assert(test::instruction(
		{Family::matrix, 16U, false, 0U, 0U}) ==
		0x6e02'ec20U);
	static_assert(test::instruction(
		{Family::matrix, 32U, false, 0U, 0U},
		{3U, 5U, 7U}) == 0x6e87'eca3U);
	every_register_field_and_alias();
	return 0;
}
