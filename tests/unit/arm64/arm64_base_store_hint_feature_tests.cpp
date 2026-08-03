// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_store_hint_test_support.hpp"

#include <cstdio>

namespace store_hint = arm64_base_store_hint_test;

namespace {

void exact_feature_table(arm64_hint_test::Harness &harness,
	arm64_hint_test::ExpectedCounts &expected)
{
	for (const auto &variant : store_hint::variants) {
		const auto requirements = store_hint::native(variant);
		arm64_hint_test::require(
			requirements.features() ==
				arm64_hint_test::primary_requirements(
					variant.immediate).features(),
			"store-hint native gate differs from the HINT decode table");
		char name[144];
		std::snprintf(name, sizeof(name),
			"%.*s %.*s exact native feature gate",
			static_cast<int>(variant.row.size()), variant.row.data(),
			static_cast<int>(variant.name.size()), variant.name.data());
		store_hint::check(harness, expected, name, variant,
			0x8300'0000'0000'0000ULL | variant.immediate);
	}
}

} // namespace

int main()
{
	arm64_hint_test::Harness harness;
	arm64_hint_test::ExpectedCounts expected;
	exact_feature_table(harness, expected);
	arm64_hint_test::require_counts(harness, expected);
	harness.print_summary("arm64-base-store-hint-feature");
}
