// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fused_floating_test_support.hpp"

namespace test = arm64_fused_floating_test;

namespace {

template<typename Floating>
void check_form(test::common::Harness &harness, const test::Form &form,
	unsigned seed)
{
	constexpr test::Registers registers{0, 1, 2, 3};
	auto initial = test::common::state(seed);
	initial.fpsr = 0;
	test::prepare_exact<Floating>(initial, form, registers);
	const auto actual = harness.run_interpreter(
		test::instruction(form, registers), initial);
	const unsigned lanes = form.scalar ? 1U :
		form.vector_bytes / sizeof(Floating);
	const auto expected = std::bit_cast<test::basic::FloatingInteger<Floating>>(
		test::exact_result<Floating>(form));
	for (unsigned lane = 0; lane < lanes; ++lane)
		test::common::require(test::basic::lane_bits<Floating>(actual,
			registers.destination, lane) == expected);
	for (unsigned byte = form.scalar ? sizeof(Floating) : form.vector_bytes;
	     byte < 16U; ++byte)
		test::common::require(actual.vectors[registers.destination].bytes[byte] ==
			std::byte{});
	test::common::require(actual.fpsr == 0U);
}

void exact_sign_and_accumulator_semantics()
{
	test::common::Harness harness;
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		if (form.width == 16U)
			check_form<_Float16>(harness, form, ++seed);
		else if (form.width == 32U)
			check_form<float>(harness, form, ++seed);
		else
			check_form<double>(harness, form, ++seed);
	});
}

void single_rounding_is_genuinely_fused()
{
	test::common::Harness harness;
	constexpr test::Registers registers{0, 1, 2, 3};
	const test::Form scalar{true, false, false, 32U, 16U};
	auto initial = test::common::state(0);
	initial.fpsr = 0;
	test::basic::set_lane(initial, 1, 0,
		std::bit_cast<float>(0x3f80'0001U));
	test::basic::set_lane(initial, 2, 0,
		std::bit_cast<float>(0x3f80'0001U));
	test::basic::set_lane(initial, 3, 0,
		std::bit_cast<float>(0xbf80'0002U));
	const auto actual = harness.run_interpreter(
		test::instruction(scalar, registers), initial);
	test::common::require(test::basic::lane_bits<float>(actual, 0, 0) ==
		0x2880'0000U);
	harness.require_differential(test::instruction(scalar, registers), initial);
}

} // namespace

int main()
{
	exact_sign_and_accumulator_semantics();
	single_rounding_is_genuinely_fused();
}
