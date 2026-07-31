// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_convert_up_engine_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_fp8_convert_up_engine_test;

namespace {

void every_scale_format_and_representative_value()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 20'000U;
	for (const auto form : test::forms) {
		const unsigned scales = form.bfloat ? 64U : 16U;
		for (unsigned format = 0U; format < 2U; ++format) {
			for (unsigned scale = 0U; scale < scales; ++scale) {
				for (unsigned index = 0U;
				     index < test::representative_inputs.size();
				     ++index) {
					auto initial = test::state(++seed);
					initial.fpmr = form.source2 ?
						test::fpmr(1U - format, format,
							0x55U, scale) :
						test::fpmr(format, 1U - format,
							scale, 0x2aU);
					std::array<std::uint8_t, 8> values{};
					for (unsigned lane = 0U; lane < 8U; ++lane)
						values[lane] =
							test::representative_inputs[
								(index + lane * 3U) %
								test::representative_inputs.size()];
					test::fill_source(initial, {28U, 29U}, values);
					char name[192];
					std::snprintf(name, sizeof(name),
						"fp8-convert-up-rounding bfloat=%u source2=%u "
						"upper=%u format=%u scale=%u value=%u",
						static_cast<unsigned>(form.bfloat),
						static_cast<unsigned>(form.source2),
						static_cast<unsigned>(form.upper),
						format, scale, index);
					test::check(harness, name, form, {28U, 29U}, initial);
				}
			}
		}
	}
	harness.print_summary("arm64-fp8-convert-up-rounding");
}

} // namespace

int main()
{
	every_scale_format_and_representative_value();
	return 0;
}
