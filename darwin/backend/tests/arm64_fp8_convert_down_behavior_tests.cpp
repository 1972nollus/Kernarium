// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_convert_down_engine_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_fp8_convert_down_engine_test;

namespace {

void every_source_class_format_lane_and_destination_half()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 500U;
	for (const auto form : test::forms) {
		const auto inputs = test::representative_inputs(form.source_width);
		for (unsigned format = 0U; format < 2U; ++format) {
			for (unsigned index = 0U; index < inputs.size(); ++index) {
				auto initial = test::state(++seed);
				initial.fpmr = test::fpmr(format);
				std::array<std::uint32_t, 8> first{};
				std::array<std::uint32_t, 8> second{};
				for (unsigned lane = 0U; lane < test::lanes(form);
				     ++lane) {
					first[lane] =
						inputs[(index + lane) % inputs.size()];
					second[lane] =
						inputs[(index + lane * 5U + 1U) %
							inputs.size()];
				}
				test::fill_source(initial, form, {28U, 29U, 30U},
					std::span{first}.first(test::lanes(form)),
					std::span{second}.first(test::lanes(form)));
				char name[160];
				std::snprintf(name, sizeof(name),
					"fp8-convert-down-behavior source=%u upper=%u "
					"format=%u input=%u",
					form.source_width,
					static_cast<unsigned>(form.upper), format, index);
				test::check(harness, name, form, {28U, 29U, 30U},
					initial);
			}
		}
	}
	harness.print_summary("arm64-fp8-convert-down-behavior");
}

} // namespace

int main()
{
	every_source_class_format_lane_and_destination_half();
	return 0;
}
