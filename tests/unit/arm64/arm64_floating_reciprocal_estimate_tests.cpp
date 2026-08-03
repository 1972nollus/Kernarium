// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_reciprocal_test_support.hpp"

#include <array>

namespace test = arm64_floating_reciprocal_test;

namespace {

constexpr test::Registers registers{0, 1, 2};

template<typename Function>
void for_each_estimate_form(Function &&function)
{
	test::for_each_form([&](const test::Form &form) {
		if (form.operation == test::Operation::reciprocal_estimate ||
		    form.operation ==
			    test::Operation::reciprocal_sqrt_estimate)
			function(form);
	});
}

void every_standard_estimate_bin(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 0;
	for_each_estimate_form([&](const test::Form &form) {
		const auto value = test::format(form.width);
		const std::array exponents{
			1U,
			static_cast<unsigned>(value.bias - 1),
			static_cast<unsigned>(value.bias),
			static_cast<unsigned>(value.bias + 1),
			static_cast<unsigned>(
				test::maximum_exponent(value) - 1U),
		};
		for (unsigned top_fraction = 0; top_fraction < 256U;
		     ++top_fraction) {
			auto initial = test::state(++seed);
			for (unsigned lane = 0; lane < test::active_lanes(form);
			     ++lane) {
				const auto exponent =
					exponents[(top_fraction + lane) %
						exponents.size()];
				const unsigned low_bits =
					value.fraction_bits - 8U;
				const auto low_mask = test::low_mask(low_bits);
				const auto low = (static_cast<std::uint64_t>(
					top_fraction * 0x9dU + lane * 0x53U)) &
					low_mask;
				const auto fraction =
					(static_cast<std::uint64_t>(top_fraction)
					 << low_bits) |
					low;
				const auto bits = test::normal_bits(value,
					exponent, fraction,
					((top_fraction + lane) & 1U) != 0U);
				test::write_lane(
					initial.vectors[registers.first].bytes,
					lane, form.width, bits);
			}
			test::check_one(harness, form, registers, initial);
		}
	});
}

void every_subnormal_normalization_position(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 20'000;
	for_each_estimate_form([&](const test::Form &form) {
		const auto value = test::format(form.width);
		for (unsigned top_bit = 0; top_bit < value.fraction_bits;
		     ++top_bit)
			for (const bool negative : {false, true}) {
				auto initial = test::state(++seed);
				for (unsigned lane = 0;
				     lane < test::active_lanes(form); ++lane) {
					const auto fraction =
						(std::uint64_t{1} << top_bit) |
						(lane == 0U ? 0U :
						 (std::uint64_t{1} << top_bit) -
							 1U);
					test::write_lane(
						initial.vectors[registers.first].bytes,
						lane, form.width,
						test::zero_bits(value, negative) |
							fraction);
				}
				test::check_one(
					harness, form, registers, initial);
			}
	});
}

void every_reciprocal_exponent_field(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 40'000;
	for (const auto width : test::widths) {
		const test::Form form{
			test::Operation::reciprocal_exponent, true, width, 16U};
		const auto value = test::format(width);
		for (unsigned exponent = 0;
		     exponent <= test::maximum_exponent(value); ++exponent)
			for (const bool negative : {false, true}) {
				auto initial = test::state(++seed);
				std::uint64_t fraction =
					(static_cast<std::uint64_t>(exponent) *
					 0x9e37'79b9U + 0x55U) &
					test::fraction_mask(value);
				if (exponent == 0U && fraction == 0U)
					fraction = 1U;
				if (exponent ==
					    test::maximum_exponent(value) &&
				    fraction != 0U)
					fraction |= std::uint64_t{1}
						<< (value.fraction_bits - 1U);
				const auto bits =
					test::zero_bits(value, negative) |
					(static_cast<std::uint64_t>(exponent)
					 << value.fraction_bits) |
					fraction;
				test::write_lane(
					initial.vectors[registers.first].bytes,
					0, width, bits);
				test::check_one(
					harness, form, registers, initial);
			}
	}
}

[[nodiscard]] test::ExpectedEngineCounts expected_counts()
{
	test::ExpectedEngineCounts expected;
	for_each_estimate_form([&](const test::Form &form) {
		const auto value = test::format(form.width);
		expected.add(form, 0U,
			256U + value.fraction_bits * 2U);
	});
	for (const auto width : test::widths) {
		const test::Form form{
			test::Operation::reciprocal_exponent, true, width, 16U};
		const auto value = test::format(width);
		expected.add(form, 0U,
			(test::maximum_exponent(value) + 1U) * 2U);
	}
	test::require(expected.total == 9'580U,
		"estimate/exponent formula omitted a behavior case");
	return expected;
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_standard_estimate_bin(harness);
	every_subnormal_normalization_position(harness);
	every_reciprocal_exponent_field(harness);
	test::require_engine_counts(harness, expected_counts());
	harness.print_summary("arm64-floating-reciprocal-estimate");
	return 0;
}
