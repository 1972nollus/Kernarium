// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_high_narrow_test_support.hpp"

#include <array>

namespace test = arm64_simd_high_narrow_test;

namespace {

void prepare_boundary_inputs(test::arm64::CpuState &initial,
	test::Shape shape, test::Registers registers, unsigned rotation)
{
	const unsigned source_bytes = 2U << shape.size;
	const unsigned source_bits = source_bytes * 8U;
	const unsigned destination_bits = source_bits / 2U;
	const auto source_mask = test::mask(source_bits);
	const auto narrow_mask = test::mask(destination_bits);
	const auto round_bit = std::uint64_t{1} << (destination_bits - 1U);
	const auto high_bit = std::uint64_t{1} << (source_bits - 1U);
	const std::array left_values{
		std::uint64_t{0}, round_bit, source_mask, source_mask,
		std::uint64_t{1} << destination_bits, narrow_mask,
		high_bit, std::uint64_t{1},
	};
	const std::array right_values{
		std::uint64_t{0}, std::uint64_t{0}, std::uint64_t{1},
		source_mask, round_bit, std::uint64_t{1},
		source_mask, high_bit,
	};
	auto &left = initial.vectors[registers.left].bytes;
	auto &right = initial.vectors[registers.right].bytes;
	for (unsigned lane = 0; lane < 16U / source_bytes; ++lane) {
		const auto pattern = (lane + rotation) % left_values.size();
		test::write_lane(left, source_bytes, lane, left_values[pattern]);
		test::write_lane(right, source_bytes, lane, right_values[pattern]);
	}
}

void every_lane_size_operation_and_half(
	test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{27U, 28U, 29U};
	unsigned seed = 30'000U;
	for (const auto operation : test::operations)
		for (const auto shape : test::shapes)
			for (unsigned rotation = 0; rotation < 4U; ++rotation) {
				auto initial = test::state(++seed);
				prepare_boundary_inputs(
					initial, shape, registers, rotation);
				harness.check_one("simd-high-narrow-lane-behavior",
					test::instruction(operation, shape, registers),
					initial,
					test::expected_state(
						initial, operation, shape, registers),
					test::native_requirements());
			}
}

void source_and_destination_aliases(
	test::engine_tests::EngineHarness &harness)
{
	constexpr std::array aliases{
		test::Registers{3U, 3U, 4U},
		test::Registers{3U, 4U, 3U},
		test::Registers{3U, 4U, 4U},
		test::Registers{3U, 3U, 3U},
		test::Registers{31U, 30U, 29U},
	};
	unsigned seed = 40'000U;
	for (const auto operation : test::operations)
		for (const auto shape : test::shapes)
			for (const auto registers : aliases) {
				auto initial = test::state(++seed);
				prepare_boundary_inputs(initial, shape, registers, seed);
				harness.check_one("simd-high-narrow-register-alias",
					test::instruction(operation, shape, registers),
					initial,
					test::expected_state(
						initial, operation, shape, registers),
					test::native_requirements());
			}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_lane_size_operation_and_half(harness);
	source_and_destination_aliases(harness);
	test::require(harness.counts().interpreter == 216U,
		"unexpected interpreter behavior count");
	test::require(harness.counts().gadgets == 216U,
		"unexpected gadget behavior count");
	harness.print_summary("arm64-simd-high-narrow-behavior");
	return 0;
}
