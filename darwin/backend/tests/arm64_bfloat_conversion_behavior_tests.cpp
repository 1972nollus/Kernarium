// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_conversion_test_support.hpp"

namespace test = arm64_bfloat_conversion_test;

namespace {

constexpr test::Registers registers{0, 1};
constexpr std::array inputs{0x3f80'0000U, 0xbfc0'0000U,
	0x4000'0000U, 0x8000'0000U};
constexpr std::array outputs{std::uint16_t{0x3f80U}, std::uint16_t{0xbfc0U},
	std::uint16_t{0x4000U}, std::uint16_t{0x8000U}};

test::common::arm64::CpuState prepared(unsigned seed)
{
	auto state = test::common::state(seed);
	state.fpsr = 0;
	for (unsigned lane = 0; lane < inputs.size(); ++lane)
		test::set_single(state, registers.source, lane, inputs[lane]);
	return state;
}

void scalar_zeroes_inactive_bits()
{
	test::common::Harness harness;
	const auto actual = harness.run_interpreter(
		test::instruction(test::Form::scalar, registers), prepared(10));
	test::common::require(test::bfloat_lane(actual, registers.destination, 0) ==
		outputs[0]);
	for (unsigned byte = 2; byte < 16U; ++byte)
		test::common::require(actual.vectors[registers.destination].bytes[byte] ==
			std::byte{});
}

void lower_and_upper_vector_placement()
{
	test::common::Harness harness;
	auto initial = prepared(11);
	const auto lower = harness.run_interpreter(
		test::instruction(test::Form::lower, registers), initial);
	for (unsigned lane = 0; lane < outputs.size(); ++lane)
		test::common::require(test::bfloat_lane(lower,
			registers.destination, lane) == outputs[lane]);
	for (unsigned byte = 8; byte < 16U; ++byte)
		test::common::require(lower.vectors[registers.destination].bytes[byte] ==
			std::byte{});

	const auto before = initial.vectors[registers.destination].bytes;
	const auto upper = harness.run_interpreter(
		test::instruction(test::Form::upper, registers), initial);
	for (unsigned byte = 0; byte < 8U; ++byte)
		test::common::require(upper.vectors[registers.destination].bytes[byte] ==
			before[byte]);
	for (unsigned lane = 0; lane < outputs.size(); ++lane)
		test::common::require(test::bfloat_lane(upper,
			registers.destination, lane + 4U) == outputs[lane]);
}

} // namespace

int main()
{
	scalar_zeroes_inactive_bits();
	lower_and_upper_vector_placement();
}
