// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_dot_test_support.hpp"

#include <array>

namespace test = arm64_simd_dot_test;

namespace {

void every_mode_width_index_pattern_and_alias()
{
	test::engine_tests::EngineHarness harness;
	constexpr std::array register_sets{
		test::Registers{28U, 29U, 30U},
		test::Registers{1U, 1U, 2U},
		test::Registers{2U, 1U, 2U},
		test::Registers{1U, 2U, 2U},
		test::Registers{3U, 3U, 3U},
	};
	for (const auto mode : test::indexed_modes)
		for (const bool q : {false, true})
			for (unsigned index = 0; index < 4U; ++index)
				for (const auto registers : register_sets)
					for (unsigned pattern = 0; pattern < 4U; ++pattern) {
						auto initial = test::state(pattern * 37U + index + q +
							static_cast<unsigned>(mode) * 11U +
							registers.destination);
						if (pattern == 1U) {
							initial.vectors[registers.left].bytes.fill(
								std::byte{0x80});
							initial.vectors[registers.right].bytes.fill(
								std::byte{0xff});
						} else if (pattern == 2U) {
							initial.vectors[registers.left].bytes.fill(
								std::byte{0xff});
							initial.vectors[registers.right].bytes.fill(
								std::byte{0x7f});
							initial.vectors[registers.destination].bytes.fill(
								std::byte{0xff});
						} else if (pattern == 3U) {
							initial.vectors[registers.left].bytes.fill(
								std::byte{0x7f});
							initial.vectors[registers.right].bytes.fill(
								std::byte{0x80});
							for (unsigned lane = 0; lane < 4U; ++lane)
								test::write_word(
									initial.vectors[
										registers.destination].bytes,
									lane * 4U, 0xffff'fff0U + lane);
						}
						test::check(harness, "dot indexed behavior",
							test::Form::indexed, mode, q, registers, index,
							initial);
					}
	harness.print_summary("simd-dot-indexed-behavior");
}

void every_register_field_and_index()
{
	test::engine_tests::EngineHarness harness;
	for (const auto mode : test::indexed_modes)
		for (const bool q : {false, true}) {
			for (unsigned value = 0; value < 32U; ++value) {
				auto initial = test::state(
					value + q + static_cast<unsigned>(mode) * 43U);
				test::check(harness, "dot indexed destination field",
					test::Form::indexed, mode, q,
					{value, 17U, 23U}, 1U, initial);
				test::check(harness, "dot indexed first-source field",
					test::Form::indexed, mode, q,
					{19U, value, 23U}, 2U, initial);
				test::check(harness, "dot indexed element-register field",
					test::Form::indexed, mode, q,
					{19U, 17U, value}, 3U, initial);
			}
			for (unsigned index = 0; index < 4U; ++index)
				test::check(harness, "dot indexed lane field",
					test::Form::indexed, mode, q,
					{19U, 17U, 23U}, index,
					test::state(191U + index + q +
						static_cast<unsigned>(mode)));
		}
	harness.print_summary("simd-dot-indexed-registers");
}

} // namespace

int main()
{
	every_mode_width_index_pattern_and_alias();
	every_register_field_and_index();
	return 0;
}
