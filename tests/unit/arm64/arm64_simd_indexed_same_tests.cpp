// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_indexed_test_support.hpp"

#include <array>

namespace test = arm64_simd_indexed_test;

namespace {

struct Values final {
	std::uint64_t accumulator;
	std::uint64_t left;
	std::uint64_t right;
};

void modular_products_accumulation_and_lane_selection(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	unsigned seed = 50'000U;
	for (const auto family : test::indexed_same_families)
		for (const auto shape : test::indexed_vector_shapes) {
			const unsigned lane_bytes = 1U << shape.size;
			const unsigned bits = lane_bytes * 8U;
			const auto maximum = test::mask(bits);
			const std::array values{
				Values{0U, 0U, maximum},
				Values{maximum, maximum, maximum},
				Values{1U, maximum, 2U},
				Values{maximum >> 1U,
					std::uint64_t{1} << (bits - 1U), 3U},
			};
			for (const auto value : values) {
				auto initial = test::state(++seed);
				test::fill_lanes(initial, 28U, lane_bytes,
					value.accumulator);
				test::fill_lanes(initial, 29U, lane_bytes,
					value.left);
				test::fill_lanes(initial, 15U, lane_bytes,
					value.right);
				test::check_one(harness,
					"simd-indexed-same-modular",
					family, shape, {28U, 29U, 15U, 0U},
					initial, counts);
			}
			const unsigned indices =
				test::lane_count(family, shape);
			for (unsigned index = 0; index < indices; ++index) {
				auto initial = test::state(++seed);
				test::fill_lanes(initial, 28U, lane_bytes, 0U);
				test::fill_lanes(initial, 29U, lane_bytes, 1U);
				for (unsigned lane = 0; lane < indices; ++lane)
					test::set_lane(initial, 15U, lane_bytes,
						lane, lane * 0x1111U + 7U);
				test::check_one(harness,
					"simd-indexed-same-lane",
					family, shape, {28U, 29U, 15U, index},
					initial, counts);
			}
		}
}

void third_constrained_source_reads_the_dirty_resident_value(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	constexpr auto family = test::indexed_same_families[0];
	constexpr test::Shape constrained_shape{1U, false, true};
	constexpr test::Shape unrestricted_shape{2U, false, true};
	constexpr test::Registers first_pin{13U, 13U, 13U, 1U};
	constexpr test::Registers second_pin{14U, 14U, 14U, 2U};
	constexpr test::Registers dirty_unpinned{15U, 15U, 15U, 1U};
	constexpr test::Registers third_source{15U, 15U, 15U, 3U};
	constexpr std::array code{
		test::engine_tests::ProgramWord{
			.offset = 0U,
			.instruction = test::instruction(
				family, constrained_shape, first_pin)},
		test::engine_tests::ProgramWord{
			.offset = 4U,
			.instruction = test::instruction(
				family, constrained_shape, second_pin)},
		test::engine_tests::ProgramWord{
			.offset = 8U,
			.instruction = test::instruction(
				family, unrestricted_shape, dirty_unpinned)},
		test::engine_tests::ProgramWord{
			.offset = 12U,
			.instruction = test::instruction(
				family, constrained_shape, third_source)},
		test::engine_tests::ProgramWord{
			.offset = 16U, .instruction = 0xd400'0001U},
	};

	auto initial = test::state(0x71a3U);
	auto expected = test::expected_state(
		initial, family, constrained_shape, first_pin);
	expected = test::expected_state(
		expected, family, constrained_shape, second_pin);
	expected = test::expected_state(
		expected, family, unrestricted_shape, dirty_unpinned);
	expected = test::expected_state(
		expected, family, constrained_shape, third_source);
	expected.pc = test::engine_tests::engine_test_code_base + 20U;

	const auto requirements = test::native_requirements(family);
	harness.check_program({
		.name = "simd-indexed-third-constrained-dirty-resident",
		.code = std::span<const test::engine_tests::ProgramWord>{code},
		.initial = initial,
		.expected = expected,
		.native = requirements,
		.expected_instructions = code.size(),
	});
	counts.add(requirements);
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts;
	modular_products_accumulation_and_lane_selection(harness, counts);
	third_constrained_source_reads_the_dirty_resident_value(harness, counts);
	test::require_counts(harness, counts);
	harness.print_summary("arm64-simd-indexed-same");
	return 0;
}
