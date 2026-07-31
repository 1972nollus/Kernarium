// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_round_test_support.hpp"

#include <array>

namespace test = arm64_floating_round_test;

namespace {

void every_fpcr_rounding_mode(test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{0, 1};
	unsigned seed = 20'000U;
	test::for_each_form([&](const test::Form &form) {
		const auto format = test::format(form.width);
		for (unsigned rounding = 0; rounding < 4; ++rounding) {
			auto initial = test::state(++seed);
			initial.fpcr = rounding << 22;
			const bool negative = (rounding & 1U) != 0U;
			test::fill_active_lanes(initial, registers.source, form,
				test::dyadic_bits(format, 5U, -1, negative));
			harness.check_one("floating-round-fpcr-rounding-mode",
				test::instruction(form, registers), initial,
				test::expected_state(initial, form, registers),
				test::native_requirements(form));
		}
	});
}

void default_not_a_number_control(
	test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{0, 1};
	unsigned seed = 21'000U;
	test::for_each_form([&](const test::Form &form) {
		const auto format = test::format(form.width);
		for (const bool signaling : {false, true})
			for (const bool negative : {false, true}) {
				auto initial = test::state(++seed);
				initial.fpcr = test::fpcr_dn;
				test::fill_active_lanes(initial, registers.source, form,
					signaling ?
						test::signaling_nan_bits(format, negative) :
						test::quiet_nan_bits(format, negative));
				harness.check_one(
					"floating-round-default-not-a-number",
					test::instruction(form, registers), initial,
					test::expected_state(initial, form, registers),
					test::native_requirements(form));
			}
	});
}

void flush_to_zero_and_input_status(
	test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{0, 1};
	unsigned seed = 22'000U;
	test::for_each_form([&](const test::Form &form) {
		const auto format = test::format(form.width);
		for (const bool negative : {false, true}) {
			auto initial = test::state(++seed);
			initial.fpcr = form.width == 16U ?
				test::fpcr_fz16 : test::fpcr_fz;
			test::fill_active_lanes(initial, registers.source, form,
				test::minimum_subnormal_bits(format, negative));
			harness.check_one("floating-round-flush-to-zero",
				test::instruction(form, registers), initial,
				test::expected_state(initial, form, registers),
				test::native_requirements(form));
		}
	});
}

void bounded_rounding_edges(test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{0, 1};
	unsigned seed = 23'000U;
	test::for_each_form([&](const test::Form &form) {
		if (!test::is_sized(form.operation))
			return;
		const auto format = test::format(form.width);
		const unsigned bound = test::integer_size(form.operation);
		const std::array inputs{
			test::next_below_power_of_two(
				format, static_cast<int>(bound - 1U)),
			test::power_of_two_bits(
				format, static_cast<int>(bound - 1U)),
			test::power_of_two_bits(
				format, static_cast<int>(bound - 1U), true),
			test::next_above_negative_power_of_two(
				format, static_cast<int>(bound - 1U)),
		};
		for (unsigned rounding = 0; rounding < 4; ++rounding)
			for (const auto input : inputs) {
				auto initial = test::state(++seed);
				initial.fpcr = rounding << 22;
				test::fill_active_lanes(initial, registers.source, form,
					input);
				harness.check_one("floating-round-integer-bound",
					test::instruction(form, registers), initial,
					test::expected_state(initial, form, registers),
					test::native_requirements(form));
			}

		/* Double precision can represent the half-way values adjacent to
		 * the signed 32-bit limits, including a rounding-induced overflow. */
		if (form.width != 64U || bound != 32U)
			return;
		const std::array crossing_inputs{
			test::dyadic_bits(format,
				(std::uint64_t{1} << 32) - 1U, -1),
			test::dyadic_bits(format,
				(std::uint64_t{1} << 32) + 1U, -1, true),
		};
		for (unsigned rounding = 0; rounding < 4; ++rounding)
			for (const auto input : crossing_inputs) {
				auto initial = test::state(++seed);
				initial.fpcr = rounding << 22;
				test::fill_active_lanes(initial, registers.source, form,
					input);
				harness.check_one(
					"floating-round-bound-crossing",
					test::instruction(form, registers), initial,
					test::expected_state(initial, form, registers),
					test::native_requirements(form));
			}
	});
}

void scalar_merging_preserves_old_destination(
	test::engine_tests::EngineHarness &harness)
{
	using uml::backend::ArmFeature;
	constexpr std::array registers{
		test::Registers{0, 1},
		test::Registers{1, 1},
		test::Registers{31, 30},
		test::Registers{31, 31},
	};
	unsigned seed = 24'000U;
	test::for_each_form([&](const test::Form &form) {
		if (!form.scalar)
			return;
		const auto format = test::format(form.width);
		for (const auto register_case : registers) {
			auto initial = test::state(++seed);
			initial.fpcr = test::fpcr_nep |
				((seed & 3U) << 22);
			test::fill_active_lanes(initial, register_case.source, form,
				test::dyadic_bits(format, 5U, -1,
					(seed & 4U) != 0U));
			harness.check_one("floating-round-scalar-merging",
				test::instruction(form, register_case), initial,
				test::expected_state(initial, form, register_case),
				test::native_requirements(form) | ArmFeature::afp);
		}
	});
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_fpcr_rounding_mode(harness);
	default_not_a_number_control(harness);
	flush_to_zero_and_input_status(harness);
	bounded_rounding_edges(harness);
	scalar_merging_preserves_old_destination(harness);
	harness.print_summary("arm64-floating-round-control");
	return 0;
}
