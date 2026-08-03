// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_high_narrow_test_support.hpp"

#include <array>

namespace test = arm64_simd_high_narrow_test;

namespace {

void verify_lane_oracle()
{
	test::require(test::oracle_lane(0x0080U, 0U, 8U, false, false) == 0U,
		"unrounded addition oracle mismatch");
	test::require(test::oracle_lane(0x0080U, 0U, 8U, false, true) == 1U,
		"rounded addition oracle mismatch");
	test::require(test::oracle_lane(
		0xffffU, 0xffffU, 8U, false, false) == 0xffU,
		"addition carry oracle mismatch");
	test::require(test::oracle_lane(
		0xffffU, 0xffffU, 8U, false, true) == 0U,
		"rounded addition carry oracle mismatch");
	test::require(test::oracle_lane(0U, 1U, 8U, true, false) == 0xffU,
		"unrounded subtraction oracle mismatch");
	test::require(test::oracle_lane(0U, 1U, 8U, true, true) == 0U,
		"rounded subtraction oracle mismatch");
	test::require(test::oracle_lane(0U, 0x81U, 8U, true, true) == 0xffU,
		"negative rounded subtraction oracle mismatch");
}

void prepare_rounding_inputs(test::arm64::CpuState &initial,
	test::Shape shape, test::Registers registers, unsigned rotation)
{
	const unsigned source_bytes = 2U << shape.size;
	const unsigned source_bits = source_bytes * 8U;
	const unsigned destination_bits = source_bits / 2U;
	const auto source_mask = test::mask(source_bits);
	const auto round_bit = std::uint64_t{1} << (destination_bits - 1U);
	const auto narrow_mask = test::mask(destination_bits);
	const std::array left_values{
		round_bit, source_mask, std::uint64_t{0}, std::uint64_t{0},
		source_mask - round_bit + 1U, narrow_mask,
	};
	const std::array right_values{
		std::uint64_t{0}, source_mask, std::uint64_t{1}, round_bit,
		std::uint64_t{0}, std::uint64_t{1},
	};
	auto &left = initial.vectors[registers.left].bytes;
	auto &right = initial.vectors[registers.right].bytes;
	for (unsigned lane = 0; lane < 16U / source_bytes; ++lane) {
		const auto pattern = (lane + rotation) % left_values.size();
		test::write_lane(left, source_bytes, lane, left_values[pattern]);
		test::write_lane(right, source_bytes, lane, right_values[pattern]);
	}
}

void truncation_and_rounding_pairs(test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{20U, 21U, 22U};
	unsigned seed = 50'000U;
	for (const bool subtract : {false, true})
		for (const auto shape : test::shapes)
			for (unsigned rotation = 0; rotation < 4U; ++rotation) {
				auto initial = test::state(++seed);
				prepare_rounding_inputs(
					initial, shape, registers, rotation);
				for (const bool round : {false, true}) {
					const auto operation = subtract ?
						(round ? test::Operation::rounded_subtract :
							test::Operation::subtract) :
						(round ? test::Operation::rounded_add :
							test::Operation::add);
					harness.check_one(
						"simd-high-narrow-rounding-pair",
						test::instruction(operation, shape, registers),
						initial,
						test::expected_state(
							initial, operation, shape, registers),
						test::native_requirements());
				}
			}
}

} // namespace

int main()
{
	verify_lane_oracle();
	test::engine_tests::EngineHarness harness;
	truncation_and_rounding_pairs(harness);
	test::require(harness.counts().interpreter == 96U,
		"unexpected interpreter rounding count");
	test::require(harness.counts().gadgets == 96U,
		"unexpected gadget rounding count");
	harness.print_summary("arm64-simd-high-narrow-rounding");
	return 0;
}
