// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_convert_up_engine_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_fp8_convert_up_engine_test;

namespace {

void every_form_register_field_and_alias()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed{};
	constexpr std::array<std::uint8_t, 8> values{
		0x00U, 0x3cU, 0xbcU, 0x7cU,
		0xfcU, 0x7dU, 0x7eU, 0x7fU,
	};
	for (const auto form : test::forms) {
		for (unsigned destination = 0U; destination < 32U;
		     ++destination) {
			for (unsigned source = 0U; source < 32U; ++source) {
				const test::Registers registers{destination, source};
				auto initial = test::state(++seed);
				initial.fpmr = test::fpmr(0U, 1U);
				test::fill_source(initial, registers, values);
				char name[176];
				std::snprintf(name, sizeof(name),
					"fp8-convert-up-allocation bfloat=%u source2=%u "
					"upper=%u rd=%u rn=%u",
					static_cast<unsigned>(form.bfloat),
					static_cast<unsigned>(form.source2),
					static_cast<unsigned>(form.upper),
					destination, source);
				test::check(harness, name, form, registers, initial);
			}
		}
	}
	harness.print_summary("arm64-fp8-convert-up-allocation");
}

} // namespace

int main()
{
	static_assert(test::instruction({false, false, false}) ==
		0x2e21'7820U);
	static_assert(test::instruction({false, true, true}) ==
		0x6e61'7820U);
	static_assert(test::instruction({true, false, false}) ==
		0x2ea1'7820U);
	static_assert(test::instruction({true, true, true}) ==
		0x6ee1'7820U);
	every_form_register_field_and_alias();
	return 0;
}
