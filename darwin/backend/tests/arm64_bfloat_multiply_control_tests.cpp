// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_multiply_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_bfloat_multiply_test;

namespace {

[[nodiscard]] test::arm64::CpuState uniform_state(
	std::uint32_t addend, std::uint16_t left,
	std::uint16_t right, std::uint32_t fpcr, unsigned seed)
{
	auto initial = test::shared::state(seed);
	initial.fpcr = fpcr;
	initial.fpsr = 0x0800'0000U;
	for (unsigned lane = 0U; lane < 4U; ++lane) {
		test::shared::write_single(initial.vectors[28].bytes,
			lane, addend);
		test::shared::write_bfloat(initial.vectors[29].bytes,
			lane * 2U, left);
		test::shared::write_bfloat(initial.vectors[29].bytes,
			lane * 2U + 1U, left);
	}
	for (unsigned lane = 0U; lane < 8U; ++lane)
		test::shared::write_bfloat(initial.vectors[15].bytes,
			lane, right);
	return initial;
}

void require_result(std::uint32_t addend, std::uint16_t left,
	std::uint16_t right, std::uint32_t fpcr,
	std::uint32_t bits, std::uint32_t flags)
{
	const auto actual =
		test::multiply_add(addend, left, right, fpcr);
	if (actual.bits == bits && actual.flags == flags)
		return;
	std::fprintf(stderr,
		"BFMLAL oracle anchor mismatch addend=%08x left=%04x "
		"right=%04x fpcr=%08x expected=%08x/%08x "
		"actual=%08x/%08x\n",
		addend, left, right, fpcr, bits, flags,
		actual.bits, actual.flags);
	std::abort();
}

void check_explicit(test::engine_tests::EngineHarness &harness,
	test::shared::ExpectedCounts &counts, const char *name,
	std::uint32_t addend, std::uint16_t left,
	std::uint16_t right, std::uint32_t fpcr,
	std::uint32_t bits, std::uint32_t flags, unsigned seed)
{
	require_result(addend, left, right, fpcr, bits, flags);
	auto initial =
		uniform_state(addend, left, right, fpcr, seed);
	test::check(harness, name, test::Form::vector_bottom, 0U,
		{28U, 29U, 15U}, initial, counts);
}

void explicit_special_rounding_flush_and_status_cases()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	check_explicit(harness, counts, "BFMLAL invalid zero times infinity",
		0U, 0U, 0x7f80U, 0U, 0x7fc0'0000U,
		test::shared::fpsr_invalid, 10U);
	check_explicit(harness, counts,
		"BFMLAL alternative invalid suppresses status",
		0U, 0U, 0x7f80U, test::shared::fpcr_ah,
		0xffc0'0000U, 0U, 11U);
	check_explicit(harness, counts,
		"BFMLAL signaling NaN with forced default",
		0U, 0x7f81U, 0x3f80U, test::shared::fpcr_dn,
		0x7fc0'0000U, test::shared::fpsr_invalid, 12U);
	check_explicit(harness, counts,
		"BFMLAL alternative NaN priority",
		0x7fc1'0000U, 0x7fc2U, 0x7fc3U,
		test::shared::fpcr_ah, 0x7fc2'0000U, 0U, 13U);
	check_explicit(harness, counts,
		"BFMLAL alternative forced default NaN",
		0x7fc1'0000U, 0x7fc2U, 0x7fc3U,
		test::shared::fpcr_ah | test::shared::fpcr_dn,
		0xffc0'0000U, 0U, 14U);

	const auto half_ulp = test::shared::bfloat(0x1p-12F);
	check_explicit(harness, counts, "BFMLAL upward half-ULP",
		0x3f80'0000U, half_ulp, half_ulp, 1U << 22U,
		0x3f80'0001U, test::shared::fpsr_inexact, 15U);
	check_explicit(harness, counts,
		"BFMLAL alternative forces nearest",
		0x3f80'0000U, half_ulp, half_ulp,
		(1U << 22U) | test::shared::fpcr_ah,
		0x3f80'0000U, 0U, 16U);
	check_explicit(harness, counts,
		"BFMLAL silent input flush",
		0U, 1U, test::shared::bfloat(1.0F),
		test::shared::fpcr_fiz, 0U, 0U, 17U);
	check_explicit(harness, counts,
		"BFMLAL status-reporting input flush",
		0U, 1U, test::shared::bfloat(1.0F),
		test::shared::fpcr_fz, 0U,
		test::shared::fpsr_input_denormal, 18U);
	check_explicit(harness, counts, "BFMLAL finite overflow",
		0U, 0x7f7fU, 0x7f7fU, 0U, 0x7f80'0000U,
		test::shared::fpsr_overflow |
			test::shared::fpsr_inexact,
		19U);
	check_explicit(harness, counts, "BFMLAL finite underflow",
		0U, 0x0080U, 0x0080U, 0U, 0U,
		test::shared::fpsr_underflow |
			test::shared::fpsr_inexact,
		20U);
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-multiply-control-explicit");
}

void every_control_combination_form_and_index()
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
		for (unsigned index = 0U;
		     index < test::index_count(form); ++index)
			for (unsigned mask = 0U;
			     mask < (1U << controls.size()); ++mask)
				for (unsigned rounding = 0U; rounding < 4U;
				     ++rounding) {
					std::uint32_t fpcr =
						rounding << 22U;
					for (unsigned bit = 0U;
					     bit < controls.size(); ++bit)
						if ((mask &
						     (1U << bit)) != 0U)
							fpcr |= controls[bit];
					auto initial = uniform_state(
						0x3f80'0000U +
							((mask + index +
							  rounding) &
							 0x7fU),
						values[(mask + index +
							rounding) %
							values.size()],
						values[(mask * 5U + index +
							rounding + 1U) %
							values.size()],
						fpcr, ++seed);
					char name[192];
					std::snprintf(name, sizeof(name),
						"BFMLAL controls form=%u "
						"index=%u mask=%u "
						"rounding=%u",
						static_cast<unsigned>(form),
						index, mask, rounding);
					test::check(harness, name, form,
						index,
						{28U, 29U, 15U},
						initial, counts);
				}
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-multiply-control-matrix");
}

} // namespace

int main()
{
	explicit_special_rounding_flush_and_status_cases();
	every_control_combination_form_and_index();
	return 0;
}
