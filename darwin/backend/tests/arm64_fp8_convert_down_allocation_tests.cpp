// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_convert_down_engine_test_support.hpp"
#include "cpu/arm64/detail/fp8.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_fp8_convert_down_engine_test;

namespace {

void every_form_register_edge_and_alias()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed{};
	for (const auto form : test::forms) {
		for (const auto registers : test::register_edges) {
			auto initial = test::state(++seed);
			const std::array<std::uint32_t, 1> first{
				form.source_width == 16U ? 0x3c00U :
					0x3f80'0000U};
			const std::array<std::uint32_t, 1> second{
				form.source_width == 16U ? 0xc000U :
					0xc000'0000U};
			test::fill_source(initial, form, registers, first, second);
			char name[144];
			std::snprintf(name, sizeof(name),
				"fp8-convert-down-allocation source=%u upper=%u "
				"rd=%u rn=%u rm=%u",
				form.source_width, static_cast<unsigned>(form.upper),
				registers.destination, registers.first,
				registers.second);
			test::check(harness, name, form, registers, initial);
		}
	}

	harness.print_summary("arm64-fp8-convert-down-allocation");
}

} // namespace

int main()
{
	static_assert(test::instruction({16U, false}) == 0x0e42'f420U);
	static_assert(test::instruction({32U, true}) == 0x4e02'f420U);
	static_assert(!uml::backend::arm64::detail::decode_fp8(
		0x0e62'f420U).recognized);
	static_assert(!uml::backend::arm64::detail::decode_fp8(
		0x4e22'f420U).recognized);
	every_form_register_edge_and_alias();
	return 0;
}
