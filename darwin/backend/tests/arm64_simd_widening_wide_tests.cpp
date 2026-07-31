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
	const unsigned narrow_bytes = 1U << shape.size;
	const unsigned wide_bytes = narrow_bytes * 2U;
	const unsigned narrow_bits = narrow_bytes * 8U;
	const unsigned wide_bits = wide_bytes * 8U;
	const auto narrow_mask = test::mask(narrow_bits);
	const auto wide_mask = test::mask(wide_bits);
	const auto narrow_sign = std::uint64_t{1} << (narrow_bits - 1U);
	const auto wide_sign = std::uint64_t{1} << (wide_bits - 1U);
	const std::array signed_left{
		std::uint64_t{0}, std::uint64_t{1}, wide_mask, wide_sign,
		wide_sign - 1U, wide_sign + 1U, wide_mask - 1U,
		std::uint64_t{2},
	};
	const std::array signed_right{
		std::uint64_t{0}, narrow_mask, std::uint64_t{1},
		narrow_sign, narrow_sign - 1U, narrow_sign + 1U,
		narrow_mask - 1U, std::uint64_t{2},
	};
	const std::array unsigned_left{
		std::uint64_t{0}, std::uint64_t{1}, wide_mask,
		wide_mask - 1U, wide_sign, wide_sign - 1U,
		std::uint64_t{2}, wide_mask,
	};
	const std::array unsigned_right{
		narrow_mask, std::uint64_t{0}, std::uint64_t{1},
		narrow_mask - 1U, narrow_sign, narrow_sign - 1U,
		std::uint64_t{2}, narrow_mask,
	};
	const auto &left_values = unsigned_values ?
		unsigned_left : signed_left;
	const auto &right_values = unsigned_values ?
		unsigned_right : signed_right;
	auto &left = initial.vectors[registers.left].bytes;
	auto &right = initial.vectors[registers.right].bytes;
	for (unsigned lane = 0; lane < 16U / wide_bytes; ++lane)
		test::write_lane(left, wide_bytes, lane,
			left_values[(lane + rotation) % left_values.size()]);
	for (unsigned lane = 0; lane < 16U / narrow_bytes; ++lane) {
		const unsigned upper_bias =
			lane >= 8U / narrow_bytes ? 3U : 0U;
		test::write_lane(right, narrow_bytes, lane,
			right_values[(lane + rotation * 3U + upper_bias) %
				right_values.size()]);
	}
}

void every_lane_size_operation_and_half(
	test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{27U, 28U, 29U};
	unsigned seed = 60'000U;
	for (const auto operation : test::wide_operations)
		for (const bool unsigned_values : {false, true})
			for (const auto shape : test::shapes)
				for (unsigned rotation = 0; rotation < 8U;
				     ++rotation) {
					auto initial = test::state(++seed);
					prepare_inputs(initial, shape, registers,
						unsigned_values, rotation);
					harness.check_one("simd-widening-wide-behavior",
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
	unsigned seed = 70'000U;
	for (const auto operation : test::wide_operations)
		for (const bool unsigned_values : {false, true})
			for (const auto shape : test::shapes)
				for (const auto registers : aliases) {
					const auto initial = test::state(++seed);
					harness.check_one("simd-widening-wide-alias",
						test::instruction(operation, unsigned_values,
							shape, registers),
						initial,
						test::expected_state(initial, operation,
							unsigned_values, shape, registers),
						test::native_requirements());
				}
}

void known_answer(test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{0U, 1U, 2U};
	auto initial = test::state(80'000U);
	initial.vectors[1].bytes.fill(std::byte{});
	initial.vectors[2].bytes.fill(std::byte{});
	test::write_lane(initial.vectors[1].bytes, 2U, 0U, 0x8000U);
	initial.vectors[2].bytes[0] = std::byte{0xff};
	const auto expected = test::expected_state(initial,
		test::Operation::add_wide, false, {0U, false}, registers);
	test::require(test::read_lane(
		expected.vectors[0].bytes, 2U, 0U) == 0x7fffU,
		"signed add-wide oracle mismatch");
	harness.check_one("simd-widening-wide-known",
		test::instruction(test::Operation::add_wide, false,
			{0U, false}, registers),
		initial, expected, test::native_requirements());
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_lane_size_operation_and_half(harness);
	source_and_destination_aliases(harness);
	known_answer(harness);
	test::require_engine_counts(harness, 313U, 313U);
	harness.print_summary("arm64-simd-widening-wide");
	return 0;
}
