// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_integer_to_floating_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_integer_to_floating_test;

namespace {

[[nodiscard]] constexpr std::uint64_t encode_magnitude(
	const test::Form &form, std::uint64_t magnitude,
	bool negative) noexcept
{
	const auto mask = test::low_mask(form.integer_width);
	magnitude &= mask;
	return negative ? (~magnitude + 1U) & mask : magnitude;
}

void every_rounding_direction_and_tie(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		const auto precision =
			test::format(form.destination_width).fraction_bits + 1U;
		const auto mask = test::low_mask(form.integer_width);
		const auto tie_even = precision < form.integer_width ?
			(std::uint64_t{1} << precision) + 1U : 3U;
		const auto tie_odd = precision + 1U < form.integer_width ?
			(std::uint64_t{1} << precision) + 3U : mask / 3U;
		const std::array magnitudes{
			std::uint64_t{1}, tie_even, tie_odd,
			form.unsigned_source ? mask :
				test::sign_mask(form.integer_width) - 1U};
		for (unsigned rounding = 0U; rounding < 4U; ++rounding)
			for (const bool negative : {false, true}) {
				if (negative && form.unsigned_source)
					continue;
				for (unsigned value = 0U;
				     value < magnitudes.size(); ++value) {
					const test::Registers registers{3U, 4U};
					auto initial = test::state(++seed);
					initial.fpcr = rounding << 22;
					test::fill_source(initial, form, registers,
						encode_magnitude(form,
							magnitudes[value], negative),
						false);
					char name[160];
					std::snprintf(name, sizeof(name),
						"integer-to-floating-rounding class=%u "
						"iw=%u fw=%u unsigned=%u mode=%u "
						"negative=%u value=%u",
						static_cast<unsigned>(form.encoding),
						form.integer_width,
						form.destination_width,
						static_cast<unsigned>(
							form.unsigned_source),
						rounding,
						static_cast<unsigned>(negative),
						value);
					test::check(harness, name, form,
						registers, initial);
				}
			}
	}, false);
}

void half_overflow_in_every_direction(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 10'000U;
	test::for_each_form([&](const test::Form &form) {
		if (form.destination_width != 16U)
			return;
		const auto maximum = form.unsigned_source ?
			test::low_mask(form.integer_width) :
			test::sign_mask(form.integer_width) - 1U;
		for (unsigned rounding = 0U; rounding < 4U; ++rounding)
			for (const bool negative : {false, true}) {
				if (negative && form.unsigned_source)
					continue;
				const test::Registers registers{8U, 12U};
				auto initial = test::state(++seed);
				initial.fpcr = rounding << 22;
				test::fill_source(initial, form, registers,
					encode_magnitude(form, maximum, negative),
					false);
				test::check(harness,
					"integer-to-floating-half-overflow-direction",
					form, registers, initial);
			}
	}, false);
}

void half_subnormal_rounding_boundaries(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 20'000U;
	for (const bool unsigned_source : {false, true})
		for (const unsigned integer_width : {32U, 64U}) {
			const test::Form form{
				test::Encoding::scalar_fixed, unsigned_source, true,
				integer_width, 16U, 2U, integer_width};
			/*
			 * At fbits=32/64 these magnitudes cover zero-after-rounding,
			 * a midpoint, and adjacent retained-bit parity classes.
			 */
			const unsigned shift = integer_width - 24U;
			const std::array magnitudes{
				std::uint64_t{1},
				std::uint64_t{1} << (shift - 1U),
				(std::uint64_t{1} << shift) +
					(std::uint64_t{1} << (shift - 1U)),
				(std::uint64_t{2} << shift) +
					(std::uint64_t{1} << (shift - 1U)),
			};
			for (unsigned rounding = 0U; rounding < 4U; ++rounding)
				for (const bool negative : {false, true}) {
					if (negative && unsigned_source)
						continue;
					for (const auto magnitude : magnitudes) {
						const test::Registers registers{18U, 19U};
						auto initial = test::state(++seed);
						initial.fpcr = rounding << 22;
						test::fill_source(initial, form, registers,
							encode_magnitude(form,
								magnitude, negative),
							false);
						test::check(harness,
							"integer-to-floating-half-subnormal-rounding",
							form, registers, initial);
					}
				}
		}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_rounding_direction_and_tie(harness);
	half_overflow_in_every_direction(harness);
	half_subnormal_rounding_boundaries(harness);
	harness.print_summary("arm64-integer-to-floating-rounding");
	return 0;
}
