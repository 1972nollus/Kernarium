// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_precision_conversion_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_floating_precision_conversion_test;

namespace {

void round_odd_ignores_fpcr_direction_and_jams_every_boundary()
{
	test::engine_tests::EngineHarness harness;
	constexpr std::array inputs{
		0x3ff0'0000'0000'0000ULL,
		0x3ff0'0000'0000'0001ULL,
		0x3ff0'0000'1000'0000ULL,
		0x3ff0'0000'1fff'ffffULL,
		0xbff0'0000'0000'0001ULL,
		0xbff0'0000'1000'0000ULL,
		0x7fef'ffff'ffff'ffffULL,
		0x3690'0000'0000'0000ULL,
		0x0000'0000'0000'0001ULL,
	};
	unsigned seed = 3'000U;
	for (const auto form : test::round_odd_forms) {
		for (unsigned mode = 0U; mode < 4U; ++mode) {
			for (const auto input : inputs) {
				auto initial = test::state(++seed);
				initial.fpcr = mode << 22;
				const std::array values{input};
				test::fill_source(initial, form, {28U, 29U}, values);
				char name[160];
				std::snprintf(name, sizeof(name),
					"floating-round-odd-boundary scalar=%u "
					"upper=%u mode=%u input=%llx",
					static_cast<unsigned>(form.scalar),
					static_cast<unsigned>(form.upper), mode,
					static_cast<unsigned long long>(input));
				test::check(harness, name, form, {28U, 29U}, initial);
			}
		}
	}
	harness.print_summary("arm64-simd-fcvtxn-rounding");
}

} // namespace

int main()
{
	round_odd_ignores_fpcr_direction_and_jams_every_boundary();
	return 0;
}
