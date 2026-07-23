// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_multiply_test_support.hpp"

namespace test = arm64_bfloat_multiply_test;

int main()
{
	test::common::Harness harness;
	auto initial = test::common::state(100);
	for (unsigned lane = 0; lane < 4U; ++lane)
		test::set_single(initial, 0, lane,
			std::bit_cast<std::uint32_t>(10.0F * (lane + 1U)));
	const std::array<float, 8> left{1.0F, -1.0F, 2.0F, -2.0F,
		3.0F, -3.0F, 4.0F, -4.0F};
	const std::array<float, 8> right{5.0F, 2.0F, 6.0F, 3.0F,
		7.0F, 4.0F, 8.0F, 5.0F};
	for (unsigned lane = 0; lane < 8U; ++lane) {
		test::set_bfloat(initial, 1, lane, test::bfloat(left[lane]));
		test::set_bfloat(initial, 2, lane, test::bfloat(right[lane]));
	}

	auto bottom = harness.run_interpreter(
		test::instruction(test::Form::vector_bottom), initial);
	test::require_lanes(bottom, 0, {std::bit_cast<std::uint32_t>(15.0F),
		std::bit_cast<std::uint32_t>(32.0F),
		std::bit_cast<std::uint32_t>(51.0F),
		std::bit_cast<std::uint32_t>(72.0F)});
	harness.require_differential(
		test::instruction(test::Form::vector_bottom), initial);

	auto top = harness.run_interpreter(
		test::instruction(test::Form::vector_top), initial);
	test::require_lanes(top, 0, {std::bit_cast<std::uint32_t>(8.0F),
		std::bit_cast<std::uint32_t>(14.0F),
		std::bit_cast<std::uint32_t>(18.0F),
		std::bit_cast<std::uint32_t>(20.0F)});
	harness.require_differential(
		test::instruction(test::Form::vector_top), initial);

	/* Destination/source aliasing must read every source before the write. */
	auto alias = test::common::state(101);
	for (unsigned lane = 0; lane < 8U; ++lane)
		test::set_bfloat(alias, 1, lane, test::bfloat(1.0F));
	harness.require_differential(test::instruction(test::Form::vector_bottom,
		0U, {1, 1, 1}), alias);
}
