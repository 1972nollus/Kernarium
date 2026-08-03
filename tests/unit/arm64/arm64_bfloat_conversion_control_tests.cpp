// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_conversion_test_support.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace test = arm64_bfloat_conversion_test;

namespace {

void require_oracle(std::uint32_t input, std::uint32_t fpcr,
	std::uint16_t bits, std::uint32_t flags)
{
	const auto actual = test::convert(input, fpcr);
	if (actual.bits == bits && actual.flags == flags)
		return;
	std::fprintf(stderr,
		"reduced-precision control oracle mismatch input=%08x "
		"fpcr=%08x expected=%04x/%08x actual=%04x/%08x\n",
		input, fpcr, bits, flags, actual.bits, actual.flags);
	std::abort();
}

void run_explicit(test::engine_tests::EngineHarness &harness,
	const char *name, std::uint32_t input, std::uint32_t fpcr,
	std::uint16_t bits, std::uint32_t flags, unsigned seed)
{
	require_oracle(input, fpcr, bits, flags);
	for (const auto form : test::forms) {
		auto initial = test::state(seed +
			static_cast<unsigned>(form) * 100U);
		initial.fpcr = fpcr;
		initial.fpsr = 0x0800'0080U;
		for (unsigned lane = 0U; lane < 4U; ++lane)
			test::write_single(initial.vectors[29].bytes,
				lane, input);
		test::check(harness, name, form, {28U, 29U}, initial);
	}
}

void every_rounding_mode_and_tie(
	test::engine_tests::EngineHarness &harness)
{
	constexpr std::array positive{
		std::uint16_t{0x3f80U}, std::uint16_t{0x3f81U},
		std::uint16_t{0x3f80U}, std::uint16_t{0x3f80U}};
	constexpr std::array negative{
		std::uint16_t{0xbf80U}, std::uint16_t{0xbf80U},
		std::uint16_t{0xbf81U}, std::uint16_t{0xbf80U}};
	for (unsigned mode = 0U; mode < 4U; ++mode) {
		char positive_name[128];
		std::snprintf(positive_name, sizeof(positive_name),
			"reduced-precision-positive-tie mode=%u", mode);
		run_explicit(harness, positive_name, 0x3f80'8000U,
			mode << 22U, positive[mode],
			test::floating::fpsr_inexact, 10U + mode);
		char negative_name[128];
		std::snprintf(negative_name, sizeof(negative_name),
			"reduced-precision-negative-tie mode=%u", mode);
		run_explicit(harness, negative_name, 0xbf80'8000U,
			mode << 22U, negative[mode],
			test::floating::fpsr_inexact, 20U + mode);
	}
}

void nan_default_and_alternative_controls(
	test::engine_tests::EngineHarness &harness)
{
	run_explicit(harness, "reduced-precision-signaling-nan",
		0xff80'1234U, 0U, 0xffc0U,
		test::floating::fpsr_invalid, 30U);
	run_explicit(harness, "reduced-precision-signaling-nan-alternative",
		0xff80'1234U, test::floating::fpcr_ah,
		0xffc0U, 0U, 31U);
	run_explicit(harness, "reduced-precision-default-nan",
		0xffc1'2345U, test::floating::fpcr_dn,
		0x7fc0U, 0U, 32U);
	run_explicit(harness, "reduced-precision-default-nan-alternative",
		0xffc1'2345U,
		test::floating::fpcr_dn | test::floating::fpcr_ah,
		0xffc0U, 0U, 33U);
}

void denormal_flush_underflow_and_overflow(
	test::engine_tests::EngineHarness &harness)
{
	run_explicit(harness, "reduced-precision-exact-denormal",
		0x0001'0000U, 0U, 0x0001U, 0U, 40U);
	run_explicit(harness, "reduced-precision-inexact-denormal",
		0x0001'0001U, 0U, 0x0001U,
		test::floating::fpsr_underflow |
			test::floating::fpsr_inexact,
		41U);
	run_explicit(harness, "reduced-precision-standard-input-flush",
		0x0001'0001U, test::floating::fpcr_fz, 0x0000U,
		test::floating::fpsr_input_denormal, 42U);
	run_explicit(harness, "reduced-precision-silent-input-flush",
		0x0001'0001U, test::floating::fpcr_fiz, 0x0000U,
		0U, 43U);
	run_explicit(harness, "reduced-precision-alternative-input-flush",
		0x0001'0001U, test::floating::fpcr_ah, 0x0000U,
		0U, 44U);
	run_explicit(harness, "reduced-precision-overflow",
		0x7f7f'ffffU, 0U, 0x7f80U,
		test::floating::fpsr_overflow |
			test::floating::fpsr_inexact,
		45U);
	run_explicit(harness, "reduced-precision-overflow-toward-zero",
		0x7f7f'ffffU, 3U << 22U, 0x7f7fU,
		test::floating::fpsr_inexact, 46U);
}

void every_relevant_control_combination(
	test::engine_tests::EngineHarness &harness)
{
	constexpr std::array control_bits{
		test::floating::fpcr_fiz, test::floating::fpcr_ah,
		test::floating::fpcr_nep, test::floating::fpcr_fz,
		test::floating::fpcr_dn,
	};
	unsigned seed = 1000U;
	for (const auto form : test::forms)
		for (unsigned controls = 0U;
		     controls < (1U << control_bits.size()); ++controls) {
			std::uint32_t fpcr{};
			for (unsigned bit = 0U; bit < control_bits.size(); ++bit)
				if ((controls & (1U << bit)) != 0U)
					fpcr |= control_bits[bit];
			for (unsigned rounding = 0U; rounding < 4U;
			     ++rounding) {
				auto initial = test::state(++seed);
				initial.fpcr = fpcr | (rounding << 22U);
				for (unsigned lane = 0U; lane < 4U; ++lane)
					test::write_single(
						initial.vectors[29].bytes,
						lane,
						test::representative_inputs[
							(controls +
							 rounding +
							 lane * 3U) %
							test::representative_inputs
								.size()]);
				char name[192];
				std::snprintf(name, sizeof(name),
					"reduced-precision-controls form=%u "
					"mask=%u rounding=%u",
					static_cast<unsigned>(form),
					controls, rounding);
				test::check(harness, name, form,
					{28U, 29U}, initial);
			}
		}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_rounding_mode_and_tie(harness);
	nan_default_and_alternative_controls(harness);
	denormal_flush_underflow_and_overflow(harness);
	every_relevant_control_combination(harness);
	harness.print_summary("arm64-bfloat-conversion-control");
	return 0;
}
