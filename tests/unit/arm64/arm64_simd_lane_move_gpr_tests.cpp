// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_lane_move_test_support.hpp"

#include <array>

namespace test = arm64_simd_lane_move_test;

namespace {

constexpr std::array register_cases{
	test::Registers{18, 17},
	test::Registers{31, 17},
	test::Registers{18, 31},
	test::Registers{31, 31},
};

void every_signed_and_unsigned_lane_width_and_discard(
	test::engine_tests::EngineHarness &harness)
{
	for (unsigned immediate = 0; immediate < 32; ++immediate)
		for (const bool signed_move : {false, true})
			for (const bool wide : {false, true}) {
				if (!test::valid_gpr_move(
				    signed_move, wide, immediate))
					continue;
				const auto lane = *test::decode_lane(immediate);
				for (const auto registers : register_cases) {
					auto initial = test::state(
						immediate + wide * 31U +
						signed_move * 53U + registers.source);
					/* Exercise sign extension with a nonuniform lane. */
					initial.vectors[registers.source]
						.bytes[lane.offset +
							lane.element_bytes - 1U] |=
						std::byte{0x80};
					harness.check_one("simd-lane-to-gpr-behavior",
						test::gpr_move_instruction(signed_move, wide,
							immediate, registers),
						initial,
						test::expected_gpr_move(initial, signed_move,
							wide, immediate, registers),
						test::native_requirements());
				}
			}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	/* Preferred UMOV encodings are also MOV (to general). */
	every_signed_and_unsigned_lane_width_and_discard(harness);
	harness.print_summary("arm64-simd-lane-move-gpr");
	return 0;
}
