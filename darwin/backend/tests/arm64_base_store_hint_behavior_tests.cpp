// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_store_hint_test_support.hpp"

#include <array>
#include <cstdio>

namespace store_hint = arm64_base_store_hint_test;

namespace {

void full_state_patterns(arm64_hint_test::Harness &harness,
	arm64_hint_test::ExpectedCounts &expected)
{
	constexpr std::array seeds{
		0x8200'0000'0000'0001ULL,
		0x82ff'ffff'aaaa'5555ULL,
		0x8212'3456'789a'bcdeULL,
	};
	for (const auto &variant : store_hint::variants)
		for (std::size_t pattern = 0; pattern < seeds.size(); ++pattern) {
			char name[144];
			std::snprintf(name, sizeof(name),
				"%.*s %.*s full-state pattern=%zu",
				static_cast<int>(variant.row.size()), variant.row.data(),
				static_cast<int>(variant.name.size()), variant.name.data(),
				pattern);
			store_hint::check(harness, expected, name, variant,
				seeds[pattern] ^
					(static_cast<std::uint64_t>(
						variant.immediate) << 20));
		}
}

} // namespace

int main()
{
	arm64_hint_test::Harness harness;
	arm64_hint_test::ExpectedCounts expected;
	full_state_patterns(harness, expected);
	arm64_hint_test::require_counts(harness, expected);
	harness.print_summary("arm64-base-store-hint-behavior");
}
