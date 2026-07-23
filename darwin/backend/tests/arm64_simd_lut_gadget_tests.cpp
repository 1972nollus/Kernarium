// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_lut_test_support.hpp"

namespace test = arm64_simd_lut_test;

namespace {

void every_specialized_gadget_matches_the_safe_interpreter()
{
	test::common::Harness harness;
	unsigned seed = 300;
	for (const auto &variant : test::variants) {
		const unsigned values = 1U << variant.index_bits;
		for (unsigned rotation = 0; rotation < values; ++rotation) {
			auto initial = test::common::state(++seed);
			constexpr test::Registers registers{3, 5, 7};
			test::set_selected_indices(initial, variant, registers, rotation);
			harness.require_differential(
				test::instruction(variant, registers), initial);
		}
	}
}

void every_gadget_is_overlap_safe_including_wrapped_tables()
{
	test::common::Harness harness;
	unsigned seed = 600;
	for (const auto &variant : test::variants)
		for (const auto registers : test::register_cases)
			harness.require_differential(test::instruction(variant, registers),
				test::common::state(++seed));
}

} // namespace

int main()
{
	every_specialized_gadget_matches_the_safe_interpreter();
	every_gadget_is_overlap_safe_including_wrapped_tables();
}
