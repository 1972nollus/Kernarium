// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_dot_test_support.hpp"

#include <array>

namespace test = arm64_simd_dot_test;

namespace {

void every_mode_width_pattern_and_alias()
{
	test::engine_tests::EngineHarness harness;
	constexpr std::array register_sets{
		test::Registers{28U, 29U, 30U},
		test::Registers{1U, 1U, 2U},
		test::Registers{2U, 1U, 2U},
		test::Registers{1U, 2U, 2U},
		test::Registers{3U, 3U, 3U},
	};
	for (const auto mode : test::vector_modes)
		for (const bool q : {false, true})
			for (const auto registers : register_sets)
				for (unsigned pattern = 0; pattern < 4U; ++pattern) {
					auto initial = test::state(pattern * 31U + q +
						static_cast<unsigned>(mode) * 7U +
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
								initial.vectors[registers.destination].bytes,
								lane * 4U, 0x7fff'fff0U + lane);
					}
					test::check(harness, "dot vector behavior",
						test::Form::vector, mode, q, registers, 0U,
						initial);
				}
	harness.print_summary("simd-dot-vector-behavior");
}

void every_register_field()
{
	test::engine_tests::EngineHarness harness;
	for (const auto mode : test::vector_modes)
		for (const bool q : {false, true})
			for (unsigned value = 0; value < 32U; ++value) {
				auto initial = test::state(
					value + q + static_cast<unsigned>(mode) * 37U);
				test::check(harness, "dot vector destination field",
					test::Form::vector, mode, q,
					{value, 17U, 23U}, 0U, initial);
				test::check(harness, "dot vector first-source field",
					test::Form::vector, mode, q,
					{19U, value, 23U}, 0U, initial);
				test::check(harness, "dot vector second-source field",
					test::Form::vector, mode, q,
					{19U, 17U, value}, 0U, initial);
			}
	harness.print_summary("simd-dot-vector-registers");
}

} // namespace

int main()
{
	every_mode_width_pattern_and_alias();
	every_register_field();
	return 0;
}
