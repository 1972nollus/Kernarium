// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_widening_test_support.hpp"

#include <array>

namespace test = arm64_simd_widening_test;

namespace {

constexpr std::array aliases{
	test::Registers{3U, 3U, 4U},
	test::Registers{3U, 4U, 3U},
	test::Registers{3U, 4U, 4U},
	test::Registers{3U, 3U, 3U},
	test::Registers{31U, 30U, 29U},
};

void prepare_inputs(test::arm64::CpuState &initial, test::Shape shape,
	test::Registers registers, bool unsigned_values, unsigned rotation)
{
	const unsigned bytes = 1U << shape.size;
	const unsigned bits = bytes * 8U;
	const auto value_mask = test::mask(bits);
	const auto sign = std::uint64_t{1} << (bits - 1U);
	const std::array signed_left{
		std::uint64_t{0}, std::uint64_t{1}, value_mask, sign,
		sign - 1U, sign + 1U, value_mask - 1U, std::uint64_t{2},
	};
	const std::array signed_right{
		std::uint64_t{0}, value_mask, std::uint64_t{1}, sign - 1U,
		sign, value_mask, std::uint64_t{2}, sign,
	};
	const std::array unsigned_left{
		std::uint64_t{0}, std::uint64_t{1}, value_mask,
		value_mask - 1U, sign, sign - 1U, std::uint64_t{2},
		value_mask,
	};
	const std::array unsigned_right{
		value_mask, std::uint64_t{0}, std::uint64_t{1},
		value_mask, sign - 1U, sign, value_mask - 1U,
		value_mask,
	};
	const auto &left_values = unsigned_values ?
		unsigned_left : signed_left;
	const auto &right_values = unsigned_values ?
		unsigned_right : signed_right;
	auto &left = initial.vectors[registers.left].bytes;
	auto &right = initial.vectors[registers.right].bytes;
	for (unsigned lane = 0; lane < 16U / bytes; ++lane) {
		const unsigned upper_bias =
			lane >= 8U / bytes ? 3U : 0U;
		test::write_lane(left, bytes, lane,
			left_values[(lane + rotation + upper_bias) %
				left_values.size()]);
		test::write_lane(right, bytes, lane,
			right_values[(lane + rotation * 3U + upper_bias) %
				right_values.size()]);
	}
}

void every_lane_size_operation_and_half(
	test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{27U, 28U, 29U};
	unsigned seed = 30'000U;
	for (const auto operation : test::long_operations)
		for (const bool unsigned_values : {false, true})
			for (const auto shape : test::shapes)
				for (unsigned rotation = 0; rotation < 8U;
				     ++rotation) {
					auto initial = test::state(++seed);
					prepare_inputs(initial, shape, registers,
						unsigned_values, rotation);
					harness.check_one("simd-widening-long-behavior",
						test::instruction(operation, unsigned_values,
							shape, registers),
						initial,
						test::expected_state(initial, operation,
							unsigned_values, shape, registers),
						test::native_requirements());
				}
}

void source_and_destination_aliases(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 40'000U;
	for (const auto operation : test::long_operations)
		for (const bool unsigned_values : {false, true})
			for (const auto shape : test::shapes)
				for (const auto registers : aliases) {
					const auto initial = test::state(++seed);
					harness.check_one("simd-widening-long-alias",
						test::instruction(operation, unsigned_values,
							shape, registers),
						initial,
						test::expected_state(initial, operation,
							unsigned_values, shape, registers),
						test::native_requirements());
				}
}

void known_answers(test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{0U, 1U, 2U};
	auto initial = test::state(50'000U);
	initial.vectors[1].bytes.fill(std::byte{});
	initial.vectors[2].bytes.fill(std::byte{});
	initial.vectors[1].bytes[0] = std::byte{0x80};
	initial.vectors[2].bytes[0] = std::byte{0x7f};
	auto expected = test::expected_state(initial,
		test::Operation::absolute_long, false, {0U, false}, registers);
	test::require(test::read_lane(
		expected.vectors[0].bytes, 2U, 0U) == 255U,
		"signed absolute-long oracle mismatch");
	harness.check_one("simd-widening-long-known-signed",
		test::instruction(test::Operation::absolute_long, false,
			{0U, false}, registers),
		initial, expected, test::native_requirements());

	initial.vectors[1].bytes[0] = std::byte{0};
	initial.vectors[2].bytes[0] = std::byte{0xff};
	expected = test::expected_state(initial,
		test::Operation::subtract_long, true, {0U, false}, registers);
	test::require(test::read_lane(
		expected.vectors[0].bytes, 2U, 0U) == 0xff01U,
		"unsigned subtract-long oracle mismatch");
	harness.check_one("simd-widening-long-known-unsigned",
		test::instruction(test::Operation::subtract_long, true,
			{0U, false}, registers),
		initial, expected, test::native_requirements());
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_lane_size_operation_and_half(harness);
	source_and_destination_aliases(harness);
	known_answers(harness);
	test::require_engine_counts(harness, 470U, 470U);
	harness.print_summary("arm64-simd-widening-long");
	return 0;
}
