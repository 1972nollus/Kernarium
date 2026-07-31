// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_scale_test_support.hpp"

#include <array>

namespace test = arm64_floating_scale_test;

namespace {

constexpr test::Registers registers{0, 1, 2};
constexpr std::uint32_t fpcr_nep = 1U << 2;
constexpr std::uint32_t fpcr_ahp = 1U << 26;

void check(test::engine_tests::EngineHarness &harness, const char *name,
	const test::Form &form, std::uint64_t input, std::int64_t scale,
	std::uint32_t fpcr, std::uint32_t fpsr, unsigned seed)
{
	auto initial = test::state(seed);
	initial.fpcr = fpcr;
	initial.fpsr = fpsr;
	test::fill_lanes(initial, registers.first, form, input);
	test::fill_lanes(initial, registers.second, form,
		test::scale_bits(scale, form.width));
	harness.check_one(name, test::instruction(form, registers), initial,
		test::expected_state(initial, form, registers),
		test::native_requirements());
}

void every_overflow_rounding_direction(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	for (const auto &form : test::forms) {
		const auto format = test::format(form.width);
		for (const bool negative : {false, true})
			for (unsigned rounding = 0; rounding < 4U; ++rounding)
				check(harness, "floating-scale-overflow-rounding",
					form,
					test::maximum_finite_bits(format, negative),
					1, rounding << 22, 0, ++seed);
	}
}

void every_underflow_rounding_direction(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 100U;
	for (const auto &form : test::forms) {
		const auto format = test::format(form.width);
		for (const bool negative : {false, true})
			for (unsigned rounding = 0; rounding < 4U; ++rounding)
				check(harness, "floating-scale-underflow-rounding",
					form,
					test::minimum_normal_bits(format, negative),
					-static_cast<std::int64_t>(
						format.fraction_bits + 1U),
					rounding << 22, 0, ++seed);
	}
}

void input_and_output_flush_modes(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 200U;
	for (const auto &form : test::forms) {
		const auto format = test::format(form.width);
		const auto output_flush = form.width == 16U ?
			test::fpcr_fz16 : test::fpcr_fz;
		for (const auto fpcr : std::array{
			     0U, output_flush, test::fpcr_ah,
			     output_flush | test::fpcr_ah})
			check(harness, "floating-scale-output-flush",
				form, test::minimum_normal_bits(format), -1,
				fpcr, test::fpsr_saturation, ++seed);

		if (form.width == 16U)
			continue;
		for (const auto fpcr : std::array{
			     0U, test::fpcr_fz, test::fpcr_fiz,
			     test::fpcr_ah, test::fpcr_ah | test::fpcr_fz,
			     test::fpcr_ah | test::fpcr_fiz})
			check(harness, "floating-scale-input-flush",
				form, test::minimum_subnormal_bits(format),
				static_cast<std::int64_t>(format.fraction_bits),
				fpcr, test::fpsr_saturation, ++seed);
	}
}

void every_nan_control_sign_and_class(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 300U;
	for (const auto &form : test::forms) {
		const auto format = test::format(form.width);
		for (const bool signaling : {false, true})
			for (const bool negative : {false, true})
				for (const auto fpcr : std::array{
				     0U, test::fpcr_dn, test::fpcr_ah,
				     test::fpcr_dn | test::fpcr_ah})
					check(harness, "floating-scale-not-a-number-control",
						form,
						signaling ?
							test::signaling_nan_bits(
								format, negative) :
							test::quiet_nan_bits(
								format, negative),
						4, fpcr, test::fpsr_saturation, ++seed);
	}
}

void ignored_and_preserved_control_state(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 500U;
	for (const auto &form : test::forms) {
		const auto format = test::format(form.width);
		check(harness, "floating-scale-preserves-cumulative-status",
			form,
			test::normal_bits(format,
				static_cast<unsigned>(format.bias), 0U),
			0, fpcr_nep | fpcr_ahp,
			test::fpsr_invalid | test::fpsr_overflow |
				test::fpsr_underflow | test::fpsr_inexact |
				test::fpsr_input_denormal |
				test::fpsr_saturation,
			++seed);
	}

	for (const auto &form : {test::Form{16U, 8U},
	     test::Form{16U, 16U}}) {
		const auto format = test::format(form.width);
		check(harness, "floating-scale-half-ignores-alternative-format",
			form, test::infinity_bits(format), 1, fpcr_ahp, 0,
			++seed);
		check(harness, "floating-scale-half-nan-ignores-alternative-format",
			form, test::signaling_nan_bits(format), 1,
			fpcr_ahp, 0, ++seed);
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_overflow_rounding_direction(harness);
	every_underflow_rounding_direction(harness);
	input_and_output_flush_modes(harness);
	every_nan_control_sign_and_class(harness);
	ignored_and_preserved_control_state(harness);
	harness.print_summary("arm64-floating-scale-control");
	return 0;
}
