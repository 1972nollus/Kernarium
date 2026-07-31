// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_convert_down_engine_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_fp8_convert_down_engine_test;

namespace {

[[nodiscard]] constexpr std::array<std::uint32_t, 10>
rounding_inputs(unsigned width) noexcept
{
	if (width == 16U)
		return {0x3c00U, 0x3c40U, 0x3c80U, 0x3cc0U,
			0x3d80U, 0xbc40U, 0x0001U, 0x0400U,
			0x7bffU, 0xfbffU};
	return {0x3f80'0000U, 0x3f90'0000U, 0x3fa0'0000U,
		0x3fb0'0000U, 0x3fc0'0000U, 0xbf90'0000U,
		0x0000'0001U, 0x0080'0000U,
		0x7f7f'ffffU, 0xff7f'ffffU};
}

void ties_scales_underflow_overflow_and_saturation()
{
	test::engine_tests::EngineHarness harness;
	constexpr std::array half_scales{-16, -1, 0, 1, 15};
	constexpr std::array single_scales{-128, -8, -1, 0, 1, 8, 127};
	unsigned seed = 1'500U;
	for (const auto form : test::forms) {
		for (unsigned format = 0U; format < 2U; ++format) {
			const auto inputs = rounding_inputs(form.source_width);
			const auto run_scale = [&](int scale, bool saturate) {
				for (const auto input : inputs) {
					auto initial = test::state(++seed);
					initial.fpmr =
						test::fpmr(format, scale, saturate);
					const std::array first{input};
					const std::array second{
						input ^ (std::uint32_t{1} <<
							(form.source_width - 1U))};
					test::fill_source(initial, form,
						{28U, 29U, 30U}, first, second);
					char name[176];
					std::snprintf(name, sizeof(name),
						"fp8-convert-down-rounding source=%u "
						"upper=%u format=%u scale=%d sat=%u "
						"input=%x",
						form.source_width,
						static_cast<unsigned>(form.upper),
						format, scale,
						static_cast<unsigned>(saturate), input);
					test::check(harness, name, form,
						{28U, 29U, 30U}, initial);
				}
			};
			if (form.source_width == 16U)
				for (const auto scale : half_scales) {
					run_scale(scale, false);
					run_scale(scale, true);
				}
			else
				for (const auto scale : single_scales) {
					run_scale(scale, false);
					run_scale(scale, true);
				}
		}
	}
	harness.print_summary("arm64-fp8-convert-down-rounding");
}

} // namespace

int main()
{
	ties_scales_underflow_overflow_and_saturation();
	return 0;
}
