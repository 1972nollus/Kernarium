// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_indexed_test_support.hpp"

#include <array>

namespace test = arm64_simd_indexed_test;

namespace {

[[nodiscard]] unsigned pick_register(unsigned first,
	unsigned second = 32U, unsigned limit = 32U)
{
	for (unsigned reg = 0; reg < limit; ++reg)
		if (reg != first && reg != second)
			return reg;
	std::abort();
}

void encoding_examples()
{
	test::require(test::instruction(
		{test::Operation::smull, test::Encoding::vector_long},
		{0U, false, false}, {0U, 1U, 2U}) == 0x0e22'c020U,
		"SMULL vector encoding mismatch");
	test::require(test::instruction(
		{test::Operation::umull, test::Encoding::vector_long},
		{1U, false, true}, {3U, 4U, 5U}) == 0x6e65'c083U,
		"UMULL2 vector encoding mismatch");
	test::require(test::instruction(
		{test::Operation::smlal, test::Encoding::vector_long},
		{2U, false, false}, {6U, 7U, 8U}) == 0x0ea8'80e6U,
		"SMLAL vector encoding mismatch");
	test::require(test::instruction(
		{test::Operation::sqdmlal, test::Encoding::vector_long},
		{1U, false, false}, {5U, 6U, 7U}) == 0x0e67'90c5U,
		"SQDMLAL vector encoding mismatch");
}

void check_valid(test::engine_tests::EngineHarness &harness,
	const test::Family &family, test::Shape shape,
	test::Registers registers, unsigned seed,
	test::ExpectedCounts &counts)
{
	const auto initial = test::state(seed);
	test::check_one(harness, "simd-multiply-allocated",
		family, shape, registers, initial, counts);
}

void every_register_field_shape_index_and_alias(
	test::engine_tests::EngineHarness &harness,
	test::ExpectedCounts &counts)
{
	constexpr std::array aliases{
		test::Registers{1U, 1U, 2U, 0U},
		test::Registers{2U, 1U, 2U, 0U},
		test::Registers{1U, 2U, 2U, 0U},
		test::Registers{3U, 3U, 3U, 0U},
		test::Registers{31U, 30U, 15U, 0U},
	};
	unsigned seed = 1000U;
	for (const auto family : test::all_families)
		for (const auto shape : test::allocated_shapes(family)) {
			const unsigned right_count =
				test::right_register_count(family, shape);
			const unsigned indices =
				test::lane_count(family, shape);
			for (unsigned destination = 0; destination < 32U;
			     ++destination) {
				const auto left = pick_register(destination);
				const auto right = pick_register(
					destination, left, right_count);
				check_valid(harness, family, shape,
					{destination, left, right,
					 destination % indices},
					++seed, counts);
			}
			for (unsigned left = 0; left < 32U; ++left) {
				const auto destination = pick_register(left);
				const auto right = pick_register(
					destination, left, right_count);
				check_valid(harness, family, shape,
					{destination, left, right, left % indices},
					++seed, counts);
			}
			for (unsigned right = 0; right < right_count; ++right) {
				const auto destination = pick_register(right);
				const auto left = pick_register(destination, right);
				check_valid(harness, family, shape,
					{destination, left, right, right % indices},
					++seed, counts);
			}
			for (unsigned index = 0; index < indices; ++index)
				check_valid(harness, family, shape,
					{28U, 29U,
					 shape.size == 1U && test::indexed(family) ?
						15U : 30U,
					 index},
					++seed, counts);
			for (auto registers : aliases) {
				if (registers.right >= right_count)
					registers.right = right_count - 1U;
				registers.index =
					(registers.destination +
					 registers.left + registers.right) % indices;
				check_valid(harness, family, shape, registers,
					++seed, counts);
			}
		}
}

} // namespace

int main()
{
	encoding_examples();
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts;
	every_register_field_shape_index_and_alias(harness, counts);
	test::require_counts(harness, counts);
	harness.print_summary("arm64-simd-indexed-allocation");
	return 0;
}
