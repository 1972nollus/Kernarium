// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_horizontal_test_support.hpp"

#include <cmath>
#include <limits>

namespace test = arm64_floating_horizontal_test;

namespace {

constexpr test::Registers registers{0, 1, 2};

void nan_number_and_signed_zero_rules()
{
	test::common::Harness harness;
	const test::Form maximum{test::Operation::maximum,
		test::Kind::scalar_binary, 32U, 16U};
	const test::Form maximum_number{test::Operation::maximum_number,
		test::Kind::scalar_binary, 32U, 16U};
	const test::Form minimum{test::Operation::minimum,
		test::Kind::scalar_binary, 32U, 16U};
	auto initial = test::common::state(500);
	initial.fpsr = 0;
	test::basic::set_lane(initial, registers.first, 0,
		std::bit_cast<float>(0x7fc0'1234U));
	test::basic::set_lane(initial, registers.second, 0, 3.0F);
	const auto max_nan = harness.run_interpreter(
		test::instruction(maximum, registers), initial);
	test::common::require(std::isnan(std::bit_cast<float>(
		test::basic::lane_bits<float>(max_nan, registers.destination, 0))));
	const auto maxnum_nan = harness.run_interpreter(
		test::instruction(maximum_number, registers), initial);
	test::common::require(test::basic::lane_bits<float>(maxnum_nan,
		registers.destination, 0) == std::bit_cast<std::uint32_t>(3.0F));

	initial.fpcr = 1U << 25;
	const auto default_nan = harness.run_interpreter(
		test::instruction(maximum, registers), initial);
	test::common::require(test::basic::lane_bits<float>(default_nan,
		registers.destination, 0) == 0x7fc0'0000U);

	initial.fpcr = 0;
	test::basic::set_lane(initial, registers.first, 0, 0.0F);
	test::basic::set_lane(initial, registers.second, 0, -0.0F);
	const auto max_zero = harness.run_interpreter(
		test::instruction(maximum, registers), initial);
	test::common::require(test::basic::lane_bits<float>(max_zero,
		registers.destination, 0) == 0x0000'0000U);
	const auto min_zero = harness.run_interpreter(
		test::instruction(minimum, registers), initial);
	test::common::require(test::basic::lane_bits<float>(min_zero,
		registers.destination, 0) == 0x8000'0000U);
}

void alternate_handling_and_invalid_exception()
{
	test::common::Harness harness;
	const test::Form maximum{test::Operation::maximum,
		test::Kind::scalar_binary, 32U, 16U};
	auto initial = test::common::state(510);
	initial.fpcr = 2U;
	initial.fpsr = 0;
	test::basic::set_lane(initial, registers.first, 0,
		std::bit_cast<float>(0x7fc0'1234U));
	test::basic::set_lane(initial, registers.second, 0, 3.0F);
	auto actual = harness.run_interpreter(
		test::instruction(maximum, registers), initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == std::bit_cast<std::uint32_t>(3.0F));

	test::basic::set_lane(initial, registers.first, 0, 0.0F);
	test::basic::set_lane(initial, registers.second, 0, -0.0F);
	actual = harness.run_interpreter(test::instruction(maximum, registers),
		initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == 0x8000'0000U);

	test::basic::set_lane(initial, registers.first, 0,
		std::bit_cast<float>(0x7f80'0001U));
	test::basic::set_lane(initial, registers.second, 0, 4.0F);
	actual = harness.run_interpreter(test::instruction(maximum, registers),
		initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == std::bit_cast<std::uint32_t>(4.0F));
	test::common::require((actual.fpsr & 1U) != 0U);
}

void arithmetic_rounding_and_exception_flags()
{
	test::common::Harness harness;
	const test::Form add{test::Operation::add,
		test::Kind::scalar_pairwise, 32U, 16U};
	auto initial = test::common::state(515);
	initial.fpsr = 0;
	test::basic::set_lane(initial, registers.first, 0, 1.0F);
	test::basic::set_lane(initial, registers.first, 1,
		std::bit_cast<float>(0x3380'0000U));
	auto actual = harness.run_interpreter(test::instruction(add, registers),
		initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == 0x3f80'0000U);
	test::common::require((actual.fpsr & (1U << 4)) != 0U);
	initial.fpcr = 1U << 22;
	actual = harness.run_interpreter(test::instruction(add, registers), initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == 0x3f80'0001U);

	initial.fpcr = 0;
	initial.fpsr = 0;
	const auto largest = std::bit_cast<float>(0x7f7f'ffffU);
	test::basic::set_lane(initial, registers.first, 0, largest);
	test::basic::set_lane(initial, registers.first, 1, largest);
	actual = harness.run_interpreter(test::instruction(add, registers), initial);
	test::common::require(test::basic::lane_bits<float>(actual,
		registers.destination, 0) == 0x7f80'0000U);
	test::common::require((actual.fpsr & 0x14U) == 0x14U);

	const test::Form difference{test::Operation::absolute_difference,
		test::Kind::scalar_binary, 32U, 16U};
	initial.fpsr = 0;
	test::basic::set_lane(initial, registers.first, 0,
		std::bit_cast<float>(0x7f80'0001U));
	test::basic::set_lane(initial, registers.second, 0, 1.0F);
	actual = harness.run_interpreter(test::instruction(difference, registers),
		initial);
	test::common::require(std::isnan(std::bit_cast<float>(
		test::basic::lane_bits<float>(actual, registers.destination, 0))));
	test::common::require((actual.fpsr & 1U) != 0U);
}

template<typename Floating>
void prepare_alternate_state(test::arm64::CpuState &cpu,
	const test::Form &form)
{
	const unsigned lanes = form.kind == test::Kind::scalar_binary ? 1U :
		form.kind == test::Kind::scalar_pairwise ? 2U :
		form.vector_bytes / sizeof(Floating);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto value = lane == 0U ? std::numeric_limits<Floating>::quiet_NaN() :
			lane == 1U ? static_cast<Floating>(-0.0) :
			static_cast<Floating>(lane + 0.5);
		test::basic::set_lane(cpu, registers.first, lane, value);
		test::basic::set_lane(cpu, registers.second, lane,
			lane == 0U ? static_cast<Floating>(3.0) :
			static_cast<Floating>(lane + 4.0));
	}
}

void every_ah_affected_gadget_form()
{
	test::common::Harness harness;
	unsigned seed = 520;
	test::for_each_form([&](const test::Form &form) {
		if (form.operation != test::Operation::maximum &&
		    form.operation != test::Operation::minimum)
			return;
		auto initial = test::common::state(++seed);
		initial.fpcr = 2U;
		initial.fpsr = 0;
		if (form.width == 16U)
			prepare_alternate_state<_Float16>(initial, form);
		else if (form.width == 32U)
			prepare_alternate_state<float>(initial, form);
		else
			prepare_alternate_state<double>(initial, form);
		harness.require_differential(test::instruction(form, registers), initial);
	});
}

template<typename Floating>
void check_nep_form(test::common::Harness &harness, const test::Form &form,
	unsigned seed)
{
	auto initial = test::common::state(seed);
	initial.fpcr = 4U;
	test::basic::set_lane(initial, registers.first, 0,
		static_cast<Floating>(1.25));
	test::basic::set_lane(initial, registers.first, 1,
		static_cast<Floating>(2.5));
	test::basic::set_lane(initial, registers.second, 0,
		static_cast<Floating>(5.0));
	const auto source = initial.vectors[registers.first].bytes;
	const auto actual = harness.run_interpreter(
		test::instruction(form, registers), initial);
	for (unsigned byte = sizeof(Floating); byte < 16U; ++byte)
		test::common::require(actual.vectors[registers.destination].bytes[byte] ==
			source[byte]);
	harness.require_differential(test::instruction(form, registers), initial);
}

void scalar_nep_merges_from_first_source()
{
	test::common::Harness harness;
	unsigned seed = 600;
	test::for_each_form([&](const test::Form &form) {
		if (form.kind != test::Kind::scalar_binary &&
		    form.kind != test::Kind::scalar_pairwise)
			return;
		if (form.width == 16U)
			check_nep_form<_Float16>(harness, form, ++seed);
		else if (form.width == 32U)
			check_nep_form<float>(harness, form, ++seed);
		else
			check_nep_form<double>(harness, form, ++seed);
	});
}

} // namespace

int main()
{
	nan_number_and_signed_zero_rules();
	alternate_handling_and_invalid_exception();
	arithmetic_rounding_and_exception_flags();
	every_ah_affected_gadget_form();
	scalar_nep_merges_from_first_source();
}
