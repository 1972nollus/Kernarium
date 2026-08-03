// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_matrix_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_bfloat_matrix_test;

namespace {

[[nodiscard]] test::arm64::CpuState matrix_state(std::uint32_t addend,
	const std::array<std::uint16_t, 8> &left,
	const std::array<std::uint16_t, 8> &right,
	std::uint32_t fpcr, unsigned seed)
{
	auto initial = test::shared::state(seed);
	initial.fpcr = fpcr;
	initial.fpsr = 0x0800'009fU;
	for (unsigned lane = 0U; lane < 4U; ++lane)
		test::shared::write_single(initial.vectors[28].bytes,
			lane, addend);
	for (unsigned lane = 0U; lane < 8U; ++lane) {
		test::shared::write_bfloat(initial.vectors[29].bytes,
			lane, left[lane]);
		test::shared::write_bfloat(initial.vectors[30].bytes,
			lane, right[lane]);
	}
	return initial;
}

void explicit_fixed_and_extended_behavior()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	const auto half_ulp = test::shared::bfloat(0x1p-12F);
	std::array<std::uint16_t, 8> left{};
	std::array<std::uint16_t, 8> right{};
	left[0] = half_ulp;
	right[0] = half_ulp;
	auto fixed = matrix_state(0x3f80'0000U, left, right,
		0U, 10U);
	const auto fixed_expected =
		test::expected_state(fixed, {28U, 29U, 30U});
	test::shared::require(test::shared::read_single(
		fixed_expected.vectors[28].bytes, 0U) == 0x3f80'0001U,
		"BFMMLA fixed round-to-odd anchor mismatch");
	test::check(harness, "BFMMLA fixed round-to-odd",
		{28U, 29U, 30U}, fixed, counts);

	auto extended = fixed;
	extended.fpcr = test::shared::fpcr_ebf;
	const auto extended_expected =
		test::expected_state(extended, {28U, 29U, 30U});
	test::shared::require(test::shared::read_single(
		extended_expected.vectors[28].bytes, 0U) == 0x3f80'0000U,
		"BFMMLA extended nearest anchor mismatch");
	test::check(harness, "BFMMLA extended nearest",
		{28U, 29U, 30U}, extended, counts);

	left.fill(0x7f7fU);
	right = {
		test::shared::bfloat(2.0F),
		test::shared::bfloat(-2.0F),
		test::shared::bfloat(2.0F),
		test::shared::bfloat(-2.0F),
		test::shared::bfloat(2.0F),
		test::shared::bfloat(-2.0F),
		test::shared::bfloat(2.0F),
		test::shared::bfloat(-2.0F),
	};
	auto cancellation = matrix_state(0U, left, right, 0U, 11U);
	const auto fixed_cancel =
		test::expected_state(cancellation, {28U, 29U, 30U});
	test::shared::require(test::shared::read_single(
		fixed_cancel.vectors[28].bytes, 0U) == 0x7fc0'0000U,
		"BFMMLA fixed cancellation anchor mismatch");
	test::check(harness, "BFMMLA fixed unfused cancellation",
		{28U, 29U, 30U}, cancellation, counts);
	cancellation.fpcr = test::shared::fpcr_ebf;
	const auto extended_cancel =
		test::expected_state(cancellation, {28U, 29U, 30U});
	test::shared::require(test::shared::read_single(
		extended_cancel.vectors[28].bytes, 0U) == 0U,
		"BFMMLA extended cancellation anchor mismatch");
	test::check(harness, "BFMMLA extended fused cancellation",
		{28U, 29U, 30U}, cancellation, counts);

	left.fill(0U);
	right.fill(0U);
	left[0] = 0x7f81U;
	right[0] = test::shared::bfloat(1.0F);
	auto nan = matrix_state(0U, left, right,
		test::shared::fpcr_ebf | test::shared::fpcr_ah,
		12U);
	const auto nan_expected =
		test::expected_state(nan, {28U, 29U, 30U});
	test::shared::require(test::shared::read_single(
		nan_expected.vectors[28].bytes, 0U) == 0xffc0'0000U,
		"BFMMLA alternative default-NaN anchor mismatch");
	test::check(harness, "BFMMLA forced default NaN sign",
		{28U, 29U, 30U}, nan, counts);

	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-matrix-control-explicit");
}

void every_relevant_control_combination()
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
	for (const bool extended : {false, true})
		for (unsigned mask = 0U;
		     mask < (1U << controls.size()); ++mask)
			for (unsigned rounding = 0U; rounding < 4U;
			     ++rounding) {
				std::uint32_t fpcr = rounding << 22U;
				if (extended)
					fpcr |= test::shared::fpcr_ebf;
				for (unsigned bit = 0U;
				     bit < controls.size(); ++bit)
					if ((mask & (1U << bit)) != 0U)
						fpcr |= controls[bit];
				std::array<std::uint16_t, 8> left{};
				std::array<std::uint16_t, 8> right{};
				for (unsigned lane = 0U; lane < 8U;
				     ++lane) {
					left[lane] = values[
						(mask + rounding +
						 lane * 3U) %
						values.size()];
					right[lane] = values[
						(mask * 5U + rounding +
						 lane * 7U + 1U) %
						values.size()];
				}
				auto initial = matrix_state(
					0x3f80'0000U +
						((mask + rounding) &
						 0x7fU),
					left, right, fpcr, ++seed);
				char name[160];
				std::snprintf(name, sizeof(name),
					"BFMMLA controls extended=%u "
					"mask=%u rounding=%u",
					static_cast<unsigned>(extended),
					mask, rounding);
				test::check(harness, name,
					{28U, 29U, 30U}, initial,
					counts);
			}
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-matrix-control-matrix");
}

void floating_trap_enables_are_ignored()
{
	test::engine_tests::EngineHarness harness;
	test::shared::ExpectedCounts counts;
	constexpr std::uint32_t all_trap_enables = 0x0000'9f00U;
	std::array<std::uint16_t, 8> left{};
	std::array<std::uint16_t, 8> right{};
	left[0] = 0U;
	right[0] = 0x7f80U;
	const auto initial = matrix_state(0U, left, right,
		all_trap_enables, 3000U);
	test::check(harness, "BFMMLA disables floating trap enables",
		{28U, 29U, 30U}, initial, counts);
	test::shared::require_counts(harness, counts);
	harness.print_summary("arm64-bfloat-matrix-disabled-traps");
}

} // namespace

int main()
{
	explicit_fixed_and_extended_behavior();
	every_relevant_control_combination();
	floating_trap_enables_are_ignored();
	return 0;
}
