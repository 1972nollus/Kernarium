// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_store_hint_test_support.hpp"

#include <array>
#include <cstdio>

namespace store_hint = arm64_base_store_hint_test;

namespace {

void encoding_examples()
{
	constexpr std::array expected{
		0xd503'261fU,
		0xd503'263fU,
		0xd503'269fU,
	};
	for (std::size_t index = 0; index < store_hint::variants.size(); ++index) {
		const auto &variant = store_hint::variants[index];
		arm64_hint_test::require(
			arm64_hint_test::primary_instruction(variant.immediate) ==
				expected[index],
			"store-hint encoding does not match the specification");
		arm64_hint_test::require(
			arm64_hint_test::is_allocated_primary_hint(variant.immediate),
			"store-hint encoding is missing from the allocated HINT table");
		arm64_hint_test::require(
			!arm64_hint_test::is_owned_primary_hint(variant.immediate),
			"store-hint encoding is still duplicated by generic HINT tests");
	}
}

void allocated_forms(arm64_hint_test::Harness &harness,
	arm64_hint_test::ExpectedCounts &expected)
{
	for (const auto &variant : store_hint::variants) {
		char name[128];
		std::snprintf(name, sizeof(name), "%.*s %.*s allocation",
			static_cast<int>(variant.row.size()), variant.row.data(),
			static_cast<int>(variant.name.size()), variant.name.data());
		store_hint::check(harness, expected, name, variant,
			0x8100'0000'0000'0000ULL | variant.immediate);
	}
}

} // namespace

int main()
{
	encoding_examples();
	arm64_hint_test::Harness harness;
	arm64_hint_test::ExpectedCounts expected;
	allocated_forms(harness, expected);
	arm64_hint_test::require_counts(harness, expected);
	harness.print_summary("arm64-base-store-hint-allocation");
}
