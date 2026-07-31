// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_reduce_test_support.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace test = arm64_simd_reduce_test;

namespace {

constexpr std::array register_cases{
	test::Registers{0, 1},
	test::Registers{1, 1},
	test::Registers{1, 0},
	test::Registers{31, 30},
	test::Registers{30, 31},
	test::Registers{31, 31},
};
constexpr std::array opcodes{3U, 10U, 26U, 27U};

void require_model(bool condition)
{
	if (!condition) {
		std::fputs("independent vector-reduction model check failed\n",
			stderr);
		std::abort();
	}
}

void every_operation_shape_and_alias(
	test::engine_tests::EngineHarness &harness)
{
	for (const bool q : {false, true})
		for (const bool unsigned_group : {false, true})
			for (unsigned size = 0; size < 3; ++size) {
				if (!q && size == 2U)
					continue;
				for (const auto opcode : opcodes) {
					if (opcode == 27U && unsigned_group)
						continue;
					for (const auto registers : register_cases)
						for (unsigned seed = 0; seed < 4; ++seed) {
							const auto initial = test::state(
								seed + opcode * 7U + size * 31U +
								unsigned_group * 67U + q * 101U);
							harness.check_one(test::operation_name(
								unsigned_group, opcode),
								test::instruction(q, unsigned_group,
									size, opcode, registers),
								initial,
								test::expected_state(initial, q,
									unsigned_group, size, opcode, registers),
								test::native_requirements());
						}
				}
			}
}

void byte_known_answers(test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{0, 1};
	auto initial = test::state(211);
	initial.vectors[1].bytes.fill(std::byte{});
	constexpr std::array values{
		std::uint8_t{0x80}, std::uint8_t{0x7f}, std::uint8_t{0xff},
		std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{3},
		std::uint8_t{4}, std::uint8_t{5},
	};
	for (unsigned lane = 0; lane < values.size(); ++lane)
		initial.vectors[1].bytes[lane] =
			static_cast<std::byte>(values[lane]);

	const auto check = [&](bool unsigned_group, unsigned opcode,
		std::uint64_t expected_value, std::size_t result_size) {
		const auto expected = test::expected_state(initial, false,
			unsigned_group, 0, opcode, registers);
		require_model(test::read_lane(
			expected.vectors[0].bytes, 0, result_size) == expected_value);
		for (std::size_t byte = result_size; byte < 16; ++byte)
			require_model(expected.vectors[0].bytes[byte] == std::byte{});
		harness.check_one(test::operation_name(unsigned_group, opcode),
			test::instruction(false, unsigned_group, 0, opcode, registers),
			initial, expected, test::native_requirements());
	};

	check(false, 3, 0x000dU, 2);
	check(true, 3, 0x020dU, 2);
	check(false, 10, 0x7fU, 1);
	check(true, 10, 0xffU, 1);
	check(false, 26, 0x80U, 1);
	check(true, 26, 0x01U, 1);
	check(false, 27, 0x0dU, 1);
}

void wider_destination_known_answers(
	test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{0, 1};
	auto initial = test::state(307);
	initial.vectors[1].bytes.fill(std::byte{});
	test::write_lane(initial.vectors[1].bytes, 0, 2, 0x8000U);
	test::write_lane(initial.vectors[1].bytes, 2, 2, 0x7fffU);
	test::write_lane(initial.vectors[1].bytes, 4, 2, 0xffffU);
	test::write_lane(initial.vectors[1].bytes, 6, 2, 0x0001U);

	auto expected = test::expected_state(
		initial, false, false, 1, 3, registers);
	require_model(test::read_lane(expected.vectors[0].bytes, 0, 4) ==
		0xffff'ffffU);
	harness.check_one("signed-add-long-halfword-result", test::instruction(
		false, false, 1, 3, registers), initial, expected,
		test::native_requirements());

	expected = test::expected_state(initial, false, true, 1, 3, registers);
	require_model(test::read_lane(expected.vectors[0].bytes, 0, 4) ==
		0x0001'ffffU);
	harness.check_one("unsigned-add-long-halfword-result", test::instruction(
		false, true, 1, 3, registers), initial, expected,
		test::native_requirements());

	initial.vectors[1].bytes.fill(std::byte{});
	test::write_lane(initial.vectors[1].bytes, 0, 4, 0x8000'0000U);
	test::write_lane(initial.vectors[1].bytes, 4, 4, 0x7fff'ffffU);
	test::write_lane(initial.vectors[1].bytes, 8, 4, 0xffff'ffffU);
	test::write_lane(initial.vectors[1].bytes, 12, 4, 0x0000'0001U);

	expected = test::expected_state(initial, true, false, 2, 3, registers);
	require_model(test::read_lane(expected.vectors[0].bytes, 0, 8) ==
		0xffff'ffff'ffff'ffffULL);
	harness.check_one("signed-add-long-word-result", test::instruction(
		true, false, 2, 3, registers), initial, expected,
		test::native_requirements());

	expected = test::expected_state(initial, true, true, 2, 3, registers);
	require_model(test::read_lane(expected.vectors[0].bytes, 0, 8) ==
		0x0000'0001'ffff'ffffULL);
	harness.check_one("unsigned-add-long-word-result", test::instruction(
		true, true, 2, 3, registers), initial, expected,
		test::native_requirements());

	expected = test::expected_state(initial, true, false, 2, 27, registers);
	require_model(test::read_lane(expected.vectors[0].bytes, 0, 4) ==
		0xffff'ffffU);
	harness.check_one("wrapping-add-word-result", test::instruction(
		true, false, 2, 27, registers), initial, expected,
		test::native_requirements());
}

void vector_length_and_inactive_lanes(
	test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{0, 1};
	auto initial = test::state(401);
	initial.vectors[1].bytes.fill(std::byte{0x80});
	auto expected = test::expected_state(
		initial, false, false, 0, 27, registers);
	require_model(expected.vectors[0].bytes[0] == std::byte{});
	harness.check_one("wrapping-add-eight-lanes", test::instruction(
		false, false, 0, 27, registers), initial, expected,
		test::native_requirements());

	expected = test::expected_state(
		initial, true, false, 0, 27, registers);
	require_model(expected.vectors[0].bytes[0] == std::byte{});
	harness.check_one("wrapping-add-sixteen-lanes", test::instruction(
		true, false, 0, 27, registers), initial, expected,
		test::native_requirements());

	initial.vectors[1].bytes.fill(std::byte{0xff});
	for (unsigned byte = 0; byte < 8; ++byte)
		initial.vectors[1].bytes[byte] = std::byte{1};
	expected = test::expected_state(initial, false, true, 0, 10, registers);
	require_model(expected.vectors[0].bytes[0] == std::byte{1});
	harness.check_one("eight-lane-form-ignores-upper-half",
		test::instruction(false, true, 0, 10, registers), initial, expected,
		test::native_requirements());
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_operation_shape_and_alias(harness);
	byte_known_answers(harness);
	wider_destination_known_answers(harness);
	vector_length_and_inactive_lanes(harness);
	harness.print_summary("arm64-simd-reduce-behavior");
	return 0;
}
