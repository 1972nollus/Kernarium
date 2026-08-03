// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_precision_conversion_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_floating_precision_conversion_test;

namespace {

[[nodiscard]] constexpr std::array<std::uint64_t, 10>
rounding_inputs(unsigned source_width) noexcept
{
	if (source_width == 32U)
		return {0x3f80'0000U, 0x3f80'0fffU, 0x3f80'1000U,
			0x3f80'1001U, 0x3f80'2000U, 0xbf80'0000U,
			0xbf80'0fffU, 0xbf80'1000U, 0xbf80'1001U,
			0xbf80'2000U};
	return {0x3ff0'0000'0000'0000ULL,
		0x3ff0'0000'0fff'ffffULL,
		0x3ff0'0000'1000'0000ULL,
		0x3ff0'0000'1000'0001ULL,
		0x3ff0'0000'2000'0000ULL,
		0xbff0'0000'0000'0000ULL,
		0xbff0'0000'0fff'ffffULL,
		0xbff0'0000'1000'0000ULL,
		0xbff0'0000'1000'0001ULL,
		0xbff0'0000'2000'0000ULL};
}

void all_rounding_directions_ties_and_neighbors()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 500U;
	for (const auto form : test::ordinary_forms) {
		if (form.operation != test::Operation::narrow)
			continue;
		for (unsigned mode = 0U; mode < 4U; ++mode) {
			for (const auto input : rounding_inputs(form.source_width)) {
				auto initial = test::state(++seed);
				initial.fpcr = mode << 22;
				const std::array values{input};
				test::fill_source(initial, form, {28U, 29U}, values);
				char name[160];
				std::snprintf(name, sizeof(name),
					"floating-precision-rounding source=%u "
					"destination=%u upper=%u mode=%u input=%llx",
					form.source_width, form.destination_width,
					static_cast<unsigned>(form.upper), mode,
					static_cast<unsigned long long>(input));
				test::check(harness, name, form, {28U, 29U}, initial);
			}
		}
	}
	harness.print_summary("arm64-simd-fcvt-vector-rounding");
}

} // namespace

int main()
{
	all_rounding_directions_ties_and_neighbors();
	return 0;
}
