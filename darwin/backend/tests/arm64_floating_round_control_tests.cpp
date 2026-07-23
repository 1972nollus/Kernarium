// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_round_test_support.hpp"

#include <limits>

namespace test = arm64_floating_round_test;

namespace {

template<typename Floating>
void scalar_nep_merges_old_destination(unsigned width)
{
	test::common::Harness harness;
	constexpr test::Registers registers{0, 1};
	const auto check = [&](const auto operation) {
		const test::Form form{operation, true, width, 16U};
		auto initial = test::common::state(
			width + static_cast<unsigned>(operation));
		initial.fpcr = 4U;
		initial.fpsr = 0;
		test::prepare<Floating>(initial, form, registers);
		const auto old_destination = initial.vectors[0].bytes;
		const auto actual = harness.run_interpreter(
			test::instruction(form, registers), initial);
		for (unsigned byte = sizeof(Floating); byte < 16U; ++byte)
			test::common::require(actual.vectors[0].bytes[byte] ==
				old_destination[byte]);
		harness.require_differential(test::instruction(form, registers), initial);
	};
	for (const auto operation : test::standard_operations)
		check(operation);
	if constexpr (sizeof(Floating) != 2)
		for (const auto operation : test::sized_operations)
			check(operation);
}

void exact_and_nonexact_fpsr_rules()
{
	test::common::Harness harness;
	constexpr test::Registers registers{0, 1};
	for (const auto operation : {test::Operation::nearest,
		test::Operation::exact_current, test::Operation::current,
		test::Operation::int32_exact, test::Operation::int32_zero}) {
		const test::Form form{operation, true, 32U, 16U};
		auto initial = test::common::state(0);
		initial.fpsr = 0;
		test::prepare<float>(initial, form, registers);
		const auto actual = harness.run_interpreter(
			test::instruction(form, registers), initial);
		const bool exact = operation == test::Operation::exact_current ||
			test::detail::is_sized_round(operation);
		test::common::require(((actual.fpsr >> 4) & 1U) == exact);
		harness.require_differential(test::instruction(form, registers), initial);
	}
}

void sized_invalid_returns_integer_minimum()
{
	test::common::Harness harness;
	constexpr test::Registers registers{0, 1};
	for (const auto operation : test::sized_operations) {
		const test::Form form{operation, true, 64U, 16U};
		auto initial = test::common::state(0);
		initial.fpsr = 0;
		test::basic::set_lane(initial, 1, 0,
			std::numeric_limits<double>::infinity());
		const auto actual = harness.run_interpreter(
			test::instruction(form, registers), initial);
		const unsigned integer_bits = operation == test::Operation::int32_exact ||
			operation == test::Operation::int32_zero ? 32U : 64U;
		const double expected = -std::ldexp(1.0,
			static_cast<int>(integer_bits - 1U));
		test::common::require(test::basic::lane_bits<double>(actual, 0, 0) ==
			std::bit_cast<std::uint64_t>(expected));
		test::common::require((actual.fpsr & 1U) != 0U);
		harness.require_differential(test::instruction(form, registers), initial);
	}
}

} // namespace

int main()
{
	scalar_nep_merges_old_destination<_Float16>(16U);
	scalar_nep_merges_old_destination<float>(32U);
	scalar_nep_merges_old_destination<double>(64U);
	exact_and_nonexact_fpsr_rules();
	sized_invalid_returns_integer_minimum();
}
