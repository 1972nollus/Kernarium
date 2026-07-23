// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_basic_floating_test_support.hpp"

#include <cmath>

namespace test = arm64_basic_floating_test;

namespace {

template<typename Floating>
void scalar_merging_for_width(unsigned width)
{
	test::common::Harness harness;
	constexpr test::Registers registers{0, 1, 2};
	for (const auto operation : test::scalar_operations) {
		const test::Form form{operation, true, width, 16U};
		auto initial = test::common::state(width + static_cast<unsigned>(operation));
		initial.fpcr = 4U;
		initial.fpsr = 0;
		test::prepare_exact<Floating>(initial, form, registers);
		const auto merge_source = operation == test::Operation::square_root ?
			initial.vectors[registers.destination].bytes :
			initial.vectors[registers.left].bytes;
		const auto actual = harness.run_interpreter(
			test::instruction(form, registers), initial);
		for (unsigned byte = sizeof(Floating); byte < 16U; ++byte)
			test::common::require(
				actual.vectors[registers.destination].bytes[byte] ==
				merge_source[byte]);
		harness.require_differential(test::instruction(form, registers), initial);
	}
}

template<typename Floating>
void rounding_mode_for_width(unsigned width,
	test::FloatingInteger<Floating> one_bits,
	test::FloatingInteger<Floating> half_ulp_bits)
{
	test::common::Harness harness;
	constexpr test::Registers registers{0, 1, 2};
	const test::Form form{test::Operation::add, true, width, 16U};
	auto initial = test::common::state(width);
	initial.fpcr = 1U << 22; // Round toward plus infinity.
	initial.fpsr = 0;
	test::set_lane(initial, registers.left, 0,
		std::bit_cast<Floating>(one_bits));
	test::set_lane(initial, registers.right, 0,
		std::bit_cast<Floating>(half_ulp_bits));
	const auto actual = harness.run_interpreter(
		test::instruction(form, registers), initial);
	test::common::require(test::lane_bits<Floating>(actual,
		registers.destination, 0) == one_bits + 1U);
	test::common::require((actual.fpsr & (1U << 4)) != 0U);
	harness.require_differential(test::instruction(form, registers), initial);
}

void exception_flags_are_accumulated()
{
	test::common::Harness harness;
	constexpr test::Registers registers{0, 1, 2};
	{
		const test::Form form{test::Operation::divide, true, 32U, 16U};
		auto initial = test::common::state(0);
		initial.fpsr = 0;
		test::set_lane(initial, 1, 0, 1.0F);
		test::set_lane(initial, 2, 0, 0.0F);
		const auto actual = harness.run_interpreter(
			test::instruction(form, registers), initial);
		test::common::require((actual.fpsr & (1U << 1)) != 0U);
		harness.require_differential(test::instruction(form, registers), initial);
	}
	{
		const test::Form form{test::Operation::square_root, true, 64U, 16U};
		auto initial = test::common::state(0);
		initial.fpsr = 0;
		test::set_lane(initial, 1, 0, -1.0);
		const auto actual = harness.run_interpreter(
			test::instruction(form, registers), initial);
		test::common::require((actual.fpsr & 1U) != 0U);
		harness.require_differential(test::instruction(form, registers), initial);
	}
}

} // namespace

int main()
{
	scalar_merging_for_width<_Float16>(16U);
	scalar_merging_for_width<float>(32U);
	scalar_merging_for_width<double>(64U);
	rounding_mode_for_width<_Float16>(16U, 0x3c00U, 0x1000U);
	rounding_mode_for_width<float>(32U, 0x3f80'0000U, 0x3380'0000U);
	rounding_mode_for_width<double>(64U, 0x3ff0'0000'0000'0000ULL,
		0x3ca0'0000'0000'0000ULL);
	exception_flags_are_accumulated();
}
