// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_convert_up_engine_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_fp8_convert_up_engine_test;

namespace {

void every_source_bit_pattern_format_half_and_alias()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 10'000U;
	for (const auto form : test::forms) {
		for (unsigned format = 0U; format < 2U; ++format) {
			for (unsigned group = 0U; group < 32U; ++group) {
				for (const auto registers :
				     {test::Registers{28U, 29U},
				      test::Registers{29U, 29U}}) {
					auto initial = test::state(++seed);
					initial.fpmr = form.source2 ?
						test::fpmr(1U - format, format) :
						test::fpmr(format, 1U - format);
					std::array<std::uint8_t, 16> values{};
					for (unsigned lane = 0U; lane < 8U; ++lane) {
						const auto value = static_cast<std::uint8_t>(
							group * 8U + lane);
						values[lane] = value;
						values[8U + lane] = value;
					}
					test::fill_source(initial, registers, values);
					char name[192];
					std::snprintf(name, sizeof(name),
						"fp8-convert-up-behavior bfloat=%u source2=%u "
						"upper=%u format=%u group=%u alias=%u",
						static_cast<unsigned>(form.bfloat),
						static_cast<unsigned>(form.source2),
						static_cast<unsigned>(form.upper),
						format, group,
						static_cast<unsigned>(
							registers.destination ==
							registers.source));
					test::check(harness, name, form, registers, initial);
				}
			}
		}
	}
	harness.print_summary("arm64-fp8-convert-up-behavior");
}

} // namespace

int main()
{
	every_source_bit_pattern_format_half_and_alias();
	return 0;
}
