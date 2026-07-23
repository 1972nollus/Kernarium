// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_lut_test_support.hpp"

namespace test = arm64_simd_lut_test;

namespace {

void every_segment_lane_and_table_index()
{
	test::common::Harness harness;
	constexpr test::Registers registers{0, 1, 2};
	for (const auto &variant : test::variants) {
		const unsigned values = 1U << variant.index_bits;
		for (unsigned rotation = 0; rotation < values; ++rotation) {
			auto initial = test::common::state(
				variant.part * 19U + rotation * 7U + variant.index_bits);
			test::set_selected_indices(initial, variant, registers, rotation);
			const auto expected =
				test::expected_result(initial, variant, registers);
			const auto actual = harness.run_interpreter(
				test::instruction(variant, registers), initial);
			test::common::require(
				actual.vectors[registers.destination].bytes == expected.bytes);
			test::require_only_destination_changed(
				initial, actual, registers.destination);
		}
	}
}

void source_aliases_and_table_register_wrap_are_snapshot_safe()
{
	test::common::Harness harness;
	unsigned seed = 100;
	for (const auto &variant : test::variants)
		for (const auto registers : test::register_cases) {
			auto initial = test::common::state(++seed);
			const auto expected =
				test::expected_result(initial, variant, registers);
			const auto actual = harness.run_interpreter(
				test::instruction(variant, registers), initial);
			test::common::require(
				actual.vectors[registers.destination].bytes == expected.bytes);
			test::require_only_destination_changed(
				initial, actual, registers.destination);
		}
}

} // namespace

int main()
{
	every_segment_lane_and_table_index();
	source_aliases_and_table_register_wrap_are_snapshot_safe();
}
