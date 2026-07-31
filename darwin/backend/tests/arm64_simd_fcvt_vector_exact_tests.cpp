// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_precision_conversion_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_floating_precision_conversion_test;

namespace {

void finite_special_zero_denormal_and_nan_classes()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 100U;
	for (const auto form : test::ordinary_forms) {
		const auto inputs = test::representative_inputs(form.source_width);
		for (unsigned index = 0U; index < inputs.size(); ++index) {
			auto initial = test::state(++seed);
			std::array<std::uint64_t, 4> values{};
			for (unsigned lane = 0U; lane < test::lanes(form); ++lane)
				values[lane] =
					inputs[(index + lane * 3U) % inputs.size()];
			test::fill_source(initial, form, {28U, 29U},
				std::span{values}.first(test::lanes(form)));
			char name[160];
			std::snprintf(name, sizeof(name),
				"floating-precision-exact operation=%u source=%u "
				"destination=%u upper=%u input=%u",
				static_cast<unsigned>(form.operation),
				form.source_width, form.destination_width,
				static_cast<unsigned>(form.upper), index);
			test::check(harness, name, form, {28U, 29U}, initial);
		}
	}
	harness.print_summary("arm64-simd-fcvt-vector-exact");
}

} // namespace

int main()
{
	finite_special_zero_denormal_and_nan_classes();
	return 0;
}
