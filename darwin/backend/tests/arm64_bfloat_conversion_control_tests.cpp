// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_conversion_test_support.hpp"

namespace test = arm64_bfloat_conversion_test;

namespace {

constexpr test::Registers registers{0, 1};
constexpr auto encoded = test::instruction(test::Form::scalar, registers);

test::common::arm64::CpuState scalar_state(unsigned seed, std::uint32_t input)
{
	auto state = test::common::state(seed);
	state.vectors[registers.source].bytes = {};
	state.fpsr = 0;
	test::set_single(state, registers.source, 0, input);
	return state;
}

void all_rounding_modes()
{
	test::common::Harness harness;
	constexpr std::array positive{std::uint16_t{0x3f80U},
		std::uint16_t{0x3f81U}, std::uint16_t{0x3f80U},
		std::uint16_t{0x3f80U}};
	constexpr std::array negative{std::uint16_t{0xbf80U},
		std::uint16_t{0xbf80U}, std::uint16_t{0xbf81U},
		std::uint16_t{0xbf80U}};
	for (unsigned mode = 0; mode < 4U; ++mode) {
		auto state = scalar_state(20 + mode, 0x3f80'8000U);
		state.fpcr = mode << 22;
		auto actual = harness.run_interpreter(encoded, state);
		test::common::require(test::bfloat_lane(actual,
			registers.destination, 0) == positive[mode]);
		test::common::require((actual.fpsr & (1U << 4)) != 0U);
		harness.require_differential(encoded, state);

		state = scalar_state(30 + mode, 0xbf80'8000U);
		state.fpcr = mode << 22;
		actual = harness.run_interpreter(encoded, state);
		test::common::require(test::bfloat_lane(actual,
			registers.destination, 0) == negative[mode]);
		harness.require_differential(encoded, state);
	}
}

void nan_and_alternative_controls()
{
	test::common::Harness harness;
	auto state = scalar_state(40, 0xff80'1234U);
	auto actual = harness.run_interpreter(encoded, state);
	test::common::require(test::bfloat_lane(actual,
		registers.destination, 0) == 0xffc0U);
	test::common::require((actual.fpsr & 1U) != 0U);
	harness.require_differential(encoded, state);

	state.fpcr = 2U;
	state.fpsr = 0;
	actual = harness.run_interpreter(encoded, state);
	test::common::require(test::bfloat_lane(actual,
		registers.destination, 0) == 0xffc0U);
	test::common::require(actual.fpsr == 0U);
	harness.require_differential(encoded, state);

	state.fpcr = 1U << 25;
	actual = harness.run_interpreter(encoded, state);
	test::common::require(test::bfloat_lane(actual,
		registers.destination, 0) == 0x7fc0U);
	state.fpcr = (1U << 25) | 2U;
	actual = harness.run_interpreter(encoded, state);
	test::common::require(test::bfloat_lane(actual,
		registers.destination, 0) == 0xffc0U);
}

void denormal_flush_and_tiny_rounding()
{
	test::common::Harness harness;
	auto state = scalar_state(50, 0x0001'0000U);
	auto actual = harness.run_interpreter(encoded, state);
	test::common::require(test::bfloat_lane(actual,
		registers.destination, 0) == 1U);
	test::common::require(actual.fpsr == 0U);

	state = scalar_state(51, 0x0001'0001U);
	actual = harness.run_interpreter(encoded, state);
	test::common::require(test::bfloat_lane(actual,
		registers.destination, 0) == 1U);
	test::common::require((actual.fpsr & 0x18U) == 0x18U);
	harness.require_differential(encoded, state);

	state.fpcr = 1U << 24;
	state.fpsr = 0;
	actual = harness.run_interpreter(encoded, state);
	test::common::require(test::bfloat_lane(actual,
		registers.destination, 0) == 0U);
	test::common::require((actual.fpsr & (1U << 7)) != 0U);
	harness.require_differential(encoded, state);

	state.fpcr = 1U;
	state.fpsr = 0;
	actual = harness.run_interpreter(encoded, state);
	test::common::require(test::bfloat_lane(actual,
		registers.destination, 0) == 0U);
	test::common::require(actual.fpsr == 0U);
	harness.require_differential(encoded, state);
}

void scalar_nep_merges_old_destination()
{
	test::common::Harness harness;
	auto state = scalar_state(60, 0x3f80'0000U);
	state.fpcr = 4U;
	const auto before = state.vectors[registers.destination].bytes;
	const auto actual = harness.run_interpreter(encoded, state);
	for (unsigned byte = 2U; byte < 16U; ++byte)
		test::common::require(actual.vectors[registers.destination].bytes[byte] ==
			before[byte]);
	harness.require_differential(encoded, state);
}

} // namespace

int main()
{
	all_rounding_modes();
	nan_and_alternative_controls();
	denormal_flush_and_tiny_rounding();
	scalar_nep_merges_old_destination();
}
