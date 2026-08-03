// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_dot_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_bfloat_dot_test;

namespace {

[[nodiscard]] test::arm64::CpuState uniform_state(
	std::uint32_t addend, std::uint16_t left_a,
	std::uint16_t left_b, std::uint16_t right_a,
	std::uint16_t right_b, std::uint32_t fpcr, unsigned seed)
{
	auto initial = test::shared::state(seed);
	initial.fpcr = fpcr;
	initial.fpsr = 0x0800'009fU;
	for (unsigned pair = 0U; pair < 4U; ++pair) {
		test::shared::write_single(initial.vectors[28].bytes,
			pair, addend);
		test::shared::write_bfloat(initial.vectors[29].bytes,
			pair * 2U, left_a);
		test::shared::write_bfloat(initial.vectors[29].bytes,
			pair * 2U + 1U, left_b);
		test::shared::write_bfloat(initial.vectors[30].bytes,
			pair * 2U, right_a);
		test::shared::write_bfloat(initial.vectors[30].bytes,
			pair * 2U + 1U, right_b);
	}
	return initial;
}

void explicit_fixed_and_extended_behavior()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	const auto half_ulp = test::shared::bfloat(0x1p-12F);
	const auto form = test::Form{false, true, 0U};

	auto fixed = uniform_state(0x3f80'0000U, half_ulp, 0U,
		half_ulp, 0U, 2U << 22U, 10U);
	test::shared::require(test::dot_add(0x3f80'0000U,
		half_ulp, 0U, half_ulp, 0U, fixed.fpcr) ==
			0x3f80'0001U,
		"BFDOT fixed round-to-odd anchor mismatch");
	test::check(harness, "BFDOT fixed round-to-odd", form,
		{28U, 29U, 30U}, fixed, counts);

	auto extended_nearest = fixed;
	extended_nearest.fpcr = test::shared::fpcr_ebf;
	test::shared::require(test::dot_add(0x3f80'0000U,
		half_ulp, 0U, half_ulp, 0U,
		extended_nearest.fpcr) == 0x3f80'0000U,
		"BFDOT extended RNE anchor mismatch");
	test::check(harness, "BFDOT extended nearest", form,
		{28U, 29U, 30U}, extended_nearest, counts);

	auto extended_up = extended_nearest;
	extended_up.fpcr |= 1U << 22U;
	test::shared::require(test::dot_add(0x3f80'0000U,
		half_ulp, 0U, half_ulp, 0U,
		extended_up.fpcr) == 0x3f80'0001U,
		"BFDOT extended upward anchor mismatch");
	test::check(harness, "BFDOT extended upward", form,
		{28U, 29U, 30U}, extended_up, counts);

	auto cancellation = uniform_state(0U, 0x7f7fU, 0x7f7fU,
		test::shared::bfloat(2.0F),
		test::shared::bfloat(-2.0F), 0U, 11U);
	test::shared::require(test::dot_add(0U, 0x7f7fU, 0x7f7fU,
		test::shared::bfloat(2.0F),
		test::shared::bfloat(-2.0F), 0U) == 0x7fc0'0000U,
		"BFDOT fixed overflow/cancellation anchor mismatch");
	test::check(harness, "BFDOT fixed unfused cancellation", form,
		{28U, 29U, 30U}, cancellation, counts);
	cancellation.fpcr = test::shared::fpcr_ebf;
	test::shared::require(test::dot_add(0U, 0x7f7fU, 0x7f7fU,
		test::shared::bfloat(2.0F),
		test::shared::bfloat(-2.0F),
		cancellation.fpcr) == 0U,
		"BFDOT extended fused cancellation anchor mismatch");
	test::check(harness, "BFDOT extended fused cancellation", form,
		{28U, 29U, 30U}, cancellation, counts);

	auto nan = uniform_state(0U, 0x7f81U, 0U,
		test::shared::bfloat(1.0F), 0U,
		test::shared::fpcr_ebf | test::shared::fpcr_ah,
		12U);
	test::shared::require(test::dot_add(0U, 0x7f81U, 0U,
		test::shared::bfloat(1.0F), 0U, nan.fpcr) ==
			0xffc0'0000U,
		"BFDOT alternative default-NaN anchor mismatch");
	test::check(harness, "BFDOT forced default NaN sign", form,
		{28U, 29U, 30U}, nan, counts);

	auto denormal = uniform_state(0U, 1U, 0U,
		test::shared::bfloat(1.0F), 0U, 0U, 13U);
	test::shared::require(test::dot_add(0U, 1U, 0U,
		test::shared::bfloat(1.0F), 0U, 0U) == 0U,
		"BFDOT fixed input flush anchor mismatch");
	test::check(harness, "BFDOT fixed denormal flush", form,
		{28U, 29U, 30U}, denormal, counts);

	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-dot-control-explicit");
}

void every_relevant_control_combination_and_form()
{
	constexpr std::array controls{
		test::shared::fpcr_fiz, test::shared::fpcr_ah,
		test::shared::fpcr_fz, test::shared::fpcr_dn,
	};
	constexpr std::array<std::uint16_t, 12> values{
		0x0000U, 0x8000U, 0x0001U, 0x8001U,
		0x3f80U, 0xbf80U, 0x7f7fU, 0xff7fU,
		0x7f80U, 0xff80U, 0x7fc1U, 0x7f81U,
	};
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	unsigned seed = 1000U;
	for (const auto form : test::forms)
		for (const bool extended : {false, true})
			for (unsigned mask = 0U;
			     mask < (1U << controls.size()); ++mask)
				for (unsigned rounding = 0U; rounding < 4U;
				     ++rounding) {
					std::uint32_t fpcr =
						rounding << 22U;
					if (extended)
						fpcr |= test::shared::fpcr_ebf;
					for (unsigned bit = 0U;
					     bit < controls.size(); ++bit)
						if ((mask &
						     (1U << bit)) != 0U)
							fpcr |= controls[bit];
					auto initial =
						uniform_state(
							0x3f80'0000U +
								((mask +
								  rounding) &
								 0x7fU),
							values[(mask +
								rounding) %
								values.size()],
							values[(mask * 3U +
								rounding + 1U) %
								values.size()],
							values[(mask * 5U +
								rounding + 2U) %
								values.size()],
							values[(mask * 7U +
								rounding + 3U) %
								values.size()],
							fpcr, ++seed);
					char name[192];
					std::snprintf(name, sizeof(name),
						"BFDOT controls form=%u "
						"extended=%u mask=%u "
						"rounding=%u",
						static_cast<unsigned>(
							form.by_element) *
								8U +
							static_cast<unsigned>(
								form.wide) *
								4U +
							form.index,
						static_cast<unsigned>(
							extended),
						mask, rounding);
					test::check(harness, name, form,
						{28U, 29U, 30U},
						initial, counts);
				}
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-dot-control-matrix");
}

void floating_trap_enables_are_ignored()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	constexpr std::uint32_t all_trap_enables = 0x0000'9f00U;
	auto initial = uniform_state(0U, 0U, 0U, 0x7f80U, 0U,
		all_trap_enables, 3000U);
	for (const auto form : test::forms)
		test::check(harness,
			"BFDOT disables floating trap enables", form,
			{28U, 29U, 30U}, initial, counts);
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-dot-disabled-traps");
}

} // namespace

int main()
{
	explicit_fixed_and_extended_behavior();
	every_relevant_control_combination_and_form();
	floating_trap_enables_are_ignored();
	return 0;
}
