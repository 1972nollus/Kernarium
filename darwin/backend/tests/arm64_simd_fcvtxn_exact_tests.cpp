// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_precision_conversion_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_floating_precision_conversion_test;

namespace {

void every_floating_class_and_round_odd_result()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 2'500U;
	const auto inputs = test::representative_inputs(64U);
	for (const auto form : test::round_odd_forms) {
		for (unsigned index = 0U; index < inputs.size(); ++index) {
			auto initial = test::state(++seed);
			std::array<std::uint64_t, 2> values{};
			for (unsigned lane = 0U; lane < test::lanes(form); ++lane)
				values[lane] =
					inputs[(index + lane * 7U) % inputs.size()];
			test::fill_source(initial, form, {28U, 29U},
				std::span{values}.first(test::lanes(form)));
			char name[144];
			std::snprintf(name, sizeof(name),
				"floating-round-odd-exact scalar=%u upper=%u input=%u",
				static_cast<unsigned>(form.scalar),
				static_cast<unsigned>(form.upper), index);
			test::check(harness, name, form, {28U, 29U}, initial);
		}
	}
	harness.print_summary("arm64-simd-fcvtxn-exact");
}

} // namespace

int main()
{
	every_floating_class_and_round_odd_result();
	return 0;
}
