// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_absolute_minmax_test_support.hpp"

namespace test = arm64_floating_absolute_minmax_test;

namespace {

constexpr test::Registers registers{0, 1, 2};
constexpr test::Form maximum{test::Operation::absolute_maximum, 32U, 16U};

void signaling_nan_priority_and_ioc()
{
	test::common::Harness harness;
	auto initial = test::common::state(40);
	initial.fpsr = 1U << 4;
	test::basic::set_lane(initial, registers.first, 0,
		std::bit_cast<float>(0xffc1'2345U));
	test::basic::set_lane(initial, registers.second, 0,
		std::bit_cast<float>(0x7f81'1111U));
	const auto encoded = test::instruction(maximum, registers);
	const auto actual = harness.run_interpreter(encoded, initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == 0x7fc1'1111U);
	test::common::require((actual.fpsr & 1U) != 0U);
	test::common::require((actual.fpsr & (1U << 4)) != 0U);
	harness.require_differential(encoded, initial);
}

void default_nan_is_positive_and_canonical()
{
	test::common::Harness harness;
	auto initial = test::common::state(41);
	initial.fpcr = 1U << 25;
	initial.fpsr = 0;
	test::basic::set_lane(initial, registers.first, 0,
		std::bit_cast<float>(0xffc1'2345U));
	test::basic::set_lane(initial, registers.second, 0, -1.0F);
	const auto encoded = test::instruction(maximum, registers);
	const auto actual = harness.run_interpreter(encoded, initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == 0x7fc0'0000U);
	test::common::require(actual.fpsr == 0U);
	harness.require_differential(encoded, initial);
}

void ah_and_flush_controls_are_ignored()
{
	test::common::Harness harness;
	auto initial = test::common::state(42);
	initial.fpcr = (1U << 24) | (1U << 19) | 3U;
	initial.fpsr = 0;
	test::basic::set_lane(initial, registers.first, 0,
		std::bit_cast<float>(0x8000'0001U));
	test::basic::set_lane(initial, registers.second, 0,
		std::bit_cast<float>(0x0000'0002U));
	const auto encoded = test::instruction(maximum, registers);
	const auto actual = harness.run_interpreter(encoded, initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == 0x0000'0002U);
	test::common::require((actual.fpsr & (1U << 7)) == 0U);
	harness.require_differential(encoded, initial);
}

} // namespace

int main()
{
	signaling_nan_priority_and_ioc();
	default_nan_is_positive_and_canonical();
	ah_and_flush_controls_are_ignored();
}
