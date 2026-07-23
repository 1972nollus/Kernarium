// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_scale_test_support.hpp"

namespace test = arm64_floating_scale_test;

namespace {

constexpr test::Registers registers{0, 1, 2};
constexpr test::Form single{32U, 16U};

void clear_sources(test::common::arm64::CpuState &state)
{
	state.vectors[registers.first].bytes = {};
	state.vectors[registers.second].bytes = {};
}

void extreme_scales_clamp_and_raise_expected_flags()
{
	test::common::Harness harness;
	auto initial = test::common::state(30);
	clear_sources(initial);
	initial.fpsr = 0;
	test::basic::set_lane(initial, registers.first, 0, 1.0F);
	test::basic::set_lane(initial, registers.first, 1, 1.0F);
	test::set_scale_lane<float>(initial, registers.second, 0, 0x7fff'ffffU);
	test::set_scale_lane<float>(initial, registers.second, 1, 0x8000'0000U);
	const auto encoded = test::instruction(single, registers);
	const auto actual = harness.run_interpreter(encoded, initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == 0x7f80'0000U);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 1) == 0U);
	test::common::require((actual.fpsr & 0x1cU) == 0x1cU);
	harness.require_differential(encoded, initial);
}

void directed_overflow_uses_maximum_finite()
{
	test::common::Harness harness;
	auto initial = test::common::state(31);
	clear_sources(initial);
	initial.fpcr = 3U << 22;
	initial.fpsr = 0;
	test::basic::set_lane(initial, registers.first, 0, 1.0F);
	test::set_scale_lane<float>(initial, registers.second, 0, 0x7fff'ffffU);
	const auto encoded = test::instruction(single, registers);
	const auto actual = harness.run_interpreter(encoded, initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == 0x7f7f'ffffU);
	test::common::require((actual.fpsr & 0x14U) == 0x14U);
	harness.require_differential(encoded, initial);
}

void nan_controls_and_signaling_exception()
{
	test::common::Harness harness;
	auto initial = test::common::state(32);
	clear_sources(initial);
	initial.fpsr = 1U << 4;
	test::basic::set_lane(initial, registers.first, 0,
		std::bit_cast<float>(0xff80'0123U));
	test::set_scale_lane<float>(initial, registers.second, 0, 4U);
	const auto encoded = test::instruction(single, registers);
	auto actual = harness.run_interpreter(encoded, initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == 0xffc0'0123U);
	test::common::require((actual.fpsr & 0x11U) == 0x11U);
	harness.require_differential(encoded, initial);
	initial.fpcr = (1U << 25) | 2U;
	initial.fpsr = 0;
	actual = harness.run_interpreter(encoded, initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == 0xffc0'0000U);
	test::common::require((actual.fpsr & 1U) != 0U);
	harness.require_differential(encoded, initial);
}

void input_and_output_flush_controls()
{
	test::common::Harness harness;
	const auto encoded = test::instruction(single, registers);
	auto initial = test::common::state(33);
	clear_sources(initial);
	initial.fpcr = 1U;
	initial.fpsr = 0;
	test::basic::set_lane(initial, registers.first, 0,
		std::bit_cast<float>(1U));
	test::set_scale_lane<float>(initial, registers.second, 0, 1U);
	auto actual = harness.run_interpreter(encoded, initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == 0U);
	test::common::require((actual.fpsr & (1U << 7)) == 0U);
	harness.require_differential(encoded, initial);

	initial.fpcr = 1U << 24;
	initial.fpsr = 0;
	actual = harness.run_interpreter(encoded, initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == 0U);
	test::common::require((actual.fpsr & (1U << 7)) != 0U);
	harness.require_differential(encoded, initial);

	const test::Form half{16U, 16U};
	initial = test::common::state(34);
	clear_sources(initial);
	initial.fpcr = 1U << 19;
	initial.fpsr = 0;
	test::set_scale_lane<_Float16>(initial, registers.first, 0,
		std::uint16_t{0x0400U});
	test::set_scale_lane<_Float16>(initial, registers.second, 0,
		std::uint16_t{0xffffU});
	const auto half_encoded = test::instruction(half, registers);
	actual = harness.run_interpreter(half_encoded, initial);
	test::common::require(test::basic::lane_bits<_Float16>(actual,
		registers.destination, 0) == 0U);
	test::common::require((actual.fpsr & 0x18U) == 0x08U);
	harness.require_differential(half_encoded, initial);
}

} // namespace

int main()
{
	input_and_output_flush_controls();
	extreme_scales_clamp_and_raise_expected_flags();
	directed_overflow_uses_maximum_finite();
	nan_controls_and_signaling_exception();
}
