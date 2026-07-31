// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_dot_engine_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_fp8_dot_engine_test;

namespace {

template<typename Function>
void for_each_form(Function &&function)
{
	for (const unsigned width : {16U, 32U})
		for (const bool wide : {false, true}) {
			function(test::Form{width, false, wide, 0U});
			const unsigned indices = width == 16U ? 8U : 4U;
			for (unsigned index = 0U; index < indices; ++index)
				function(test::Form{width, true, wide, index});
		}
}

void every_form_register_field_and_alias()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed{};
	for_each_form([&](const test::Form &form) {
		for (unsigned role = 0U; role < 3U; ++role) {
			const unsigned count =
				role == 2U && form.by_element &&
					form.width == 16U ? 16U : 32U;
			for (unsigned reg = 0U; reg < count; ++reg) {
				test::Registers registers{
					3U, 11U,
					form.by_element &&
						form.width == 16U ?
						7U : 17U};
				if (role == 0U)
					registers.destination = reg;
				else if (role == 1U)
					registers.first = reg;
				else
					registers.second = reg;
				auto initial = test::state(++seed);
				initial.fpmr = test::fpmr(0U, 1U, 3U);
				char name[192];
				std::snprintf(name, sizeof(name),
					"fp8-dot-allocation width=%u indexed=%u wide=%u "
					"index=%u role=%u reg=%u",
					form.width,
					static_cast<unsigned>(form.by_element),
					static_cast<unsigned>(form.wide),
					form.index, role, reg);
				test::check(harness, name, form, registers, initial);
			}
		}
		for (const auto registers : test::register_aliases) {
			if (form.by_element && form.width == 16U &&
			    registers.second >= 16U)
				continue;
			auto initial = test::state(++seed);
			initial.fpmr = test::fpmr(1U, 0U, 7U);
			test::check(harness, "fp8-dot-allocation-alias",
				form, registers, initial);
		}
	});
	harness.print_summary("arm64-fp8-dot-allocation");
}

} // namespace

int main()
{
	static_assert(test::instruction({16U, false, false, 0U}) ==
		0x0e42'fc20U);
	static_assert(test::instruction({16U, true, true, 7U},
		{3U, 5U, 7U}) == 0x4f77'08a3U);
	static_assert(test::instruction({32U, false, true, 0U}) ==
		0x4e02'fc20U);
	static_assert(test::instruction({32U, true, false, 3U},
		{3U, 5U, 31U}) == 0x0f3f'08a3U);
	every_form_register_field_and_alias();
	return 0;
}
