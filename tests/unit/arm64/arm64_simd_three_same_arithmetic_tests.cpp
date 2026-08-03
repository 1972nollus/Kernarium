// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_three_same_test_support.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace test = arm64_simd_three_same_test;

namespace {

constexpr std::array register_cases{
	test::Registers{0, 1, 2},
	test::Registers{1, 1, 2},
	test::Registers{2, 1, 2},
	test::Registers{1, 1, 1},
	test::Registers{31, 30, 29},
};

void require_model(bool condition)
{
	if (!condition) {
		std::fputs("independent three-register model check failed\n", stderr);
		std::abort();
	}
}

void every_valid_operation_and_alias(
	test::engine_tests::EngineHarness &harness)
{
	for (const bool scalar : {false, true})
		for (const bool q : {false, true}) {
			if (scalar && q)
				continue;
			for (const bool unsigned_group : {false, true})
				for (unsigned size = 0; size < 4; ++size)
					for (unsigned opcode = 0; opcode < 24; ++opcode) {
						if (opcode == 3U ||
						    !test::valid(scalar, q, unsigned_group,
								size, opcode))
							continue;
						for (const auto registers : register_cases)
							for (unsigned seed = 0; seed < 3; ++seed) {
								const auto initial = test::state(
									seed + opcode * 3U + size * 17U +
									unsigned_group * 37U + q * 53U +
									scalar * 71U);
								harness.check_one(
									"simd-three-same-behavior",
									test::instruction(scalar, q,
										unsigned_group, size, opcode,
										registers),
									initial,
									test::expected_state(initial, scalar, q,
										unsigned_group, size, opcode,
										registers),
									test::native_requirements());
							}
					}
		}
}

void known_answers(test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{0, 1, 2};
	auto initial = test::state();
	initial.vectors[0].bytes.fill(std::byte{0x5a});
	initial.vectors[1].bytes.fill(std::byte{});
	initial.vectors[2].bytes.fill(std::byte{});

	initial.vectors[2].bytes[0] = std::byte{1};
	auto expected = test::expected_state(
		initial, false, false, true, 0, 4, registers);
	require_model(expected.vectors[0].bytes[0] == std::byte{0xff});
	harness.check_one("simd-unsigned-halving-negative",
		test::instruction(false, false, true, 0, 4, registers),
		initial, expected, test::native_requirements());

	initial.vectors[1].bytes[0] = std::byte{0x81};
	initial.vectors[2].bytes[0] = std::byte{0xff};
	expected = test::expected_state(
		initial, false, false, false, 0, 10, registers);
	require_model(expected.vectors[0].bytes[0] == std::byte{0xc1});
	harness.check_one("simd-signed-rounded-right-shift",
		test::instruction(false, false, false, 0, 10, registers),
		initial, expected, test::native_requirements());

	initial.vectors[1].bytes[0] = std::byte{0x57};
	initial.vectors[2].bytes[0] = std::byte{0x83};
	expected = test::expected_state(
		initial, false, false, true, 0, 19, registers);
	require_model(expected.vectors[0].bytes[0] == std::byte{0x79});
	harness.check_one("simd-polynomial-byte-product",
		test::instruction(false, false, true, 0, 19, registers),
		initial, expected, test::native_requirements());

	initial.vectors[1].bytes = {
		std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
		std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8},
		std::byte{9}, std::byte{10}, std::byte{11}, std::byte{12},
		std::byte{13}, std::byte{14}, std::byte{15}, std::byte{16},
	};
	initial.vectors[2].bytes = {
		std::byte{17}, std::byte{18}, std::byte{19}, std::byte{20},
		std::byte{21}, std::byte{22}, std::byte{23}, std::byte{24},
		std::byte{25}, std::byte{26}, std::byte{27}, std::byte{28},
		std::byte{29}, std::byte{30}, std::byte{31}, std::byte{32},
	};
	expected = test::expected_state(
		initial, false, true, false, 0, 23, registers);
	require_model(expected.vectors[0].bytes[0] == std::byte{3});
	require_model(expected.vectors[0].bytes[7] == std::byte{31});
	require_model(expected.vectors[0].bytes[8] == std::byte{35});
	require_model(expected.vectors[0].bytes[15] == std::byte{63});
	harness.check_one("simd-pairwise-source-order",
		test::instruction(false, true, false, 0, 23, registers),
		initial, expected, test::native_requirements());
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_valid_operation_and_alias(harness);
	known_answers(harness);
	harness.print_summary("arm64-simd-three-same-arithmetic");
	return 0;
}
