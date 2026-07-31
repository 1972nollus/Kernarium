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
	const auto sign = std::uint64_t{1} << (narrow_bits - 1U);
	const std::array signed_left{
		std::uint64_t{0}, std::uint64_t{1}, narrow_mask, sign,
		sign - 1U, sign + 1U, narrow_mask - 1U, std::uint64_t{2},
	};
	const std::array signed_right{
		narrow_mask, std::uint64_t{0}, std::uint64_t{1},
		sign - 1U, sign, narrow_mask, std::uint64_t{2}, sign,
	};
	const std::array unsigned_left{
		std::uint64_t{0}, std::uint64_t{1}, narrow_mask,
		narrow_mask - 1U, sign, sign - 1U, std::uint64_t{2},
		narrow_mask,
	};
	const std::array unsigned_right{
		narrow_mask, std::uint64_t{0}, std::uint64_t{1},
		narrow_mask, sign - 1U, sign, narrow_mask - 1U,
		narrow_mask,
	};
	const std::array accumulator{
		std::uint64_t{0}, std::uint64_t{1}, wide_mask,
		wide_mask - 1U, std::uint64_t{2},
		std::uint64_t{1} << (wide_bits - 1U),
	};
	const auto &left_values = unsigned_values ?
		unsigned_left : signed_left;
	const auto &right_values = unsigned_values ?
		unsigned_right : signed_right;
	auto &left = initial.vectors[registers.left].bytes;
	auto &right = initial.vectors[registers.right].bytes;
	auto &destination =
		initial.vectors[registers.destination].bytes;
	for (unsigned lane = 0; lane < 16U / wide_bytes; ++lane)
		test::write_lane(destination, wide_bytes, lane,
			accumulator[(lane + rotation) % accumulator.size()]);
	for (unsigned lane = 0; lane < 16U / narrow_bytes; ++lane) {
		const unsigned upper_bias =
			lane >= 8U / narrow_bytes ? 3U : 0U;
		test::write_lane(left, narrow_bytes, lane,
			left_values[(lane + rotation + upper_bias) %
				left_values.size()]);
		test::write_lane(right, narrow_bytes, lane,
			right_values[(lane + rotation * 3U + upper_bias) %
				right_values.size()]);
	}
}

void every_lane_size_signedness_and_half(
	test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{27U, 28U, 29U};
	unsigned seed = 90'000U;
	for (const bool unsigned_values : {false, true})
		for (const auto shape : test::shapes)
			for (unsigned rotation = 0; rotation < 8U; ++rotation) {
				auto initial = test::state(++seed);
				prepare_inputs(initial, shape, registers,
					unsigned_values, rotation);
				harness.check_one(
					"simd-widening-absolute-accumulate-behavior",
					test::instruction(
						test::Operation::absolute_accumulate,
						unsigned_values, shape, registers),
					initial,
					test::expected_state(initial,
						test::Operation::absolute_accumulate,
						unsigned_values, shape, registers),
					test::native_requirements());
			}
}

void source_and_destination_aliases(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 100'000U;
	for (const bool unsigned_values : {false, true})
		for (const auto shape : test::shapes)
			for (const auto registers : aliases) {
				const auto initial = test::state(++seed);
				harness.check_one(
					"simd-widening-absolute-accumulate-alias",
					test::instruction(
						test::Operation::absolute_accumulate,
						unsigned_values, shape, registers),
					initial,
					test::expected_state(initial,
						test::Operation::absolute_accumulate,
						unsigned_values, shape, registers),
					test::native_requirements());
			}
}

void wrapping_known_answer(test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{0U, 1U, 2U};
	auto initial = test::state(110'000U);
	initial.vectors[0].bytes.fill(std::byte{0xff});
	initial.vectors[1].bytes.fill(std::byte{});
	initial.vectors[2].bytes.fill(std::byte{});
	initial.vectors[1].bytes[0] = std::byte{0};
	initial.vectors[2].bytes[0] = std::byte{1};
	const auto expected = test::expected_state(initial,
		test::Operation::absolute_accumulate, true,
		{0U, false}, registers);
	test::require(test::read_lane(
		expected.vectors[0].bytes, 2U, 0U) == 0U,
		"absolute-accumulate wrapping oracle mismatch");
	harness.check_one("simd-widening-absolute-accumulate-known",
		test::instruction(test::Operation::absolute_accumulate, true,
			{0U, false}, registers),
		initial, expected, test::native_requirements());
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_lane_size_signedness_and_half(harness);
	source_and_destination_aliases(harness);
	wrapping_known_answer(harness);
	test::require_engine_counts(harness, 157U, 157U);
	harness.print_summary("arm64-simd-widening-absolute");
	return 0;
}
