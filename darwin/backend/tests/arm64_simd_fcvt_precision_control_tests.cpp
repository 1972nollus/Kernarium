// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvt_precision_test_support.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace test = arm64_simd_fcvt_precision_test;

namespace {

void require_oracle(test::Pair pair, std::uint64_t input,
	std::uint32_t fpcr, std::uint64_t bits, std::uint32_t flags)
{
	const auto actual = test::oracle::convert(input, pair.source_width,
		pair.destination_width, fpcr);
	if (actual.bits == bits && actual.flags == flags)
		return;
	std::fprintf(stderr,
		"scalar precision control oracle mismatch source=%u "
		"destination=%u input=%016llx fpcr=%08x "
		"expected=%016llx/%08x actual=%016llx/%08x\n",
		pair.source_width, pair.destination_width,
		static_cast<unsigned long long>(input), fpcr,
		static_cast<unsigned long long>(bits), flags,
		static_cast<unsigned long long>(actual.bits), actual.flags);
	std::abort();
}

void run_explicit(test::engine_tests::EngineHarness &harness,
	const char *name, test::Pair pair, std::uint64_t input,
	std::uint32_t fpcr, std::uint64_t bits, std::uint32_t flags,
	unsigned seed)
{
	require_oracle(pair, input, fpcr, bits, flags);
	auto initial = test::state(seed);
	initial.fpcr = fpcr;
	initial.fpsr = 0x0800'0090U;
	test::set_source(initial, 29U, pair.source_width, input);
	test::check(harness, name, pair, {28U, 29U}, initial);
}

void nan_payload_default_nan_and_signaling(
	test::engine_tests::EngineHarness &harness)
{
	run_explicit(harness, "scalar-precision-quiet-nan", {16U, 32U},
		0x7e55U, 0U, 0x7fca'a000U, 0U, 1U);
	run_explicit(harness, "scalar-precision-signaling-nan", {16U, 32U},
		0x7d51U, 0U, 0x7fea'2000U,
		test::oracle::fpsr_invalid, 2U);

	for (const unsigned destination_width : {16U, 64U}) {
		const test::Pair pair{32U, destination_width};
		run_explicit(harness, "scalar-precision-default-nan", pair,
			test::quiet_nan(32U, true), test::oracle::fpcr_dn,
			destination_width == 16U ? 0x7e00U :
				0x7ff8'0000'0000'0000ULL,
			0U, 10U + destination_width);
		run_explicit(harness, "scalar-precision-default-nan-ah", pair,
			test::quiet_nan(32U, true),
			test::oracle::fpcr_dn | test::oracle::fpcr_ah,
			destination_width == 16U ? 0xfe00U :
				0xfff8'0000'0000'0000ULL,
			0U, 20U + destination_width);
	}
}

void source_denormal_flush_modes(
	test::engine_tests::EngineHarness &harness)
{
	struct Entry final {
		std::uint32_t fpcr;
		std::uint64_t output;
		std::uint32_t flags;
	};
	const std::array entries{
		Entry{0U, 0x36a0'0000'0000'0000ULL, 0U},
		Entry{test::oracle::fpcr_fz, 0U,
			test::oracle::fpsr_input_denormal},
		Entry{test::oracle::fpcr_fiz, 0U, 0U},
		Entry{test::oracle::fpcr_fz | test::oracle::fpcr_fiz, 0U,
			test::oracle::fpsr_input_denormal},
		Entry{test::oracle::fpcr_ah,
			0x36a0'0000'0000'0000ULL,
			test::oracle::fpsr_input_denormal},
		Entry{test::oracle::fpcr_fz | test::oracle::fpcr_ah,
			0x36a0'0000'0000'0000ULL,
			test::oracle::fpsr_input_denormal},
		Entry{test::oracle::fpcr_fiz | test::oracle::fpcr_ah,
			0U, 0U},
	};
	for (unsigned index = 0U; index < entries.size(); ++index) {
		const auto &entry = entries[index];
		char name[128];
		std::snprintf(name, sizeof(name),
			"scalar-precision-input-flush index=%u", index);
		run_explicit(harness, name, {32U, 64U}, 1U, entry.fpcr,
			entry.output, entry.flags, 100U + index);
	}
}

void output_flush_timing(
	test::engine_tests::EngineHarness &harness)
{
	for (const bool ah : {false, true}) {
		const auto fpcr = test::oracle::fpcr_fz |
			(ah ? test::oracle::fpcr_ah : 0U);
		const auto flags = test::oracle::fpsr_underflow |
			(ah ? test::oracle::fpsr_inexact : 0U);
		run_explicit(harness,
			ah ? "scalar-precision-output-flush-ah" :
				"scalar-precision-output-flush-standard",
			{64U, 32U}, 0x36a0'0000'0000'0000ULL, fpcr,
			0U, flags, 200U + ah);
	}
}

void alternative_half_source_and_destination(
	test::engine_tests::EngineHarness &harness)
{
	run_explicit(harness, "scalar-precision-alternative-half-source",
		{16U, 32U}, 0x7c00U, test::oracle::fpcr_ahp,
		0x4780'0000U, 0U, 300U);
	run_explicit(harness, "scalar-precision-alternative-half-finite",
		{32U, 16U}, 0x4780'0000U, test::oracle::fpcr_ahp,
		0x7c00U, 0U, 301U);
	run_explicit(harness, "scalar-precision-alternative-half-overflow",
		{32U, 16U}, 0x4843'5000U, test::oracle::fpcr_ahp,
		0x7fffU, test::oracle::fpsr_invalid, 302U);
	for (const bool negative : {false, true}) {
		run_explicit(harness,
			negative ?
				"scalar-precision-alternative-half-negative-infinity" :
				"scalar-precision-alternative-half-positive-infinity",
			{32U, 16U}, test::infinity(32U, negative),
			test::oracle::fpcr_ahp,
			negative ? 0xffffU : 0x7fffU,
			test::oracle::fpsr_invalid, 310U + negative);
		run_explicit(harness,
			negative ?
				"scalar-precision-alternative-half-negative-nan" :
				"scalar-precision-alternative-half-positive-nan",
			{32U, 16U}, test::quiet_nan(32U, negative),
			test::oracle::fpcr_ahp,
			negative ? 0x8000U : 0U,
			test::oracle::fpsr_invalid, 320U + negative);
	}
}

void every_relevant_control_combination(
	test::engine_tests::EngineHarness &harness)
{
	constexpr std::array control_bits{
		test::oracle::fpcr_fiz, test::oracle::fpcr_ah,
		test::oracle::fpcr_nep, test::oracle::fpcr_fz,
		test::oracle::fpcr_dn, test::oracle::fpcr_ahp,
	};
	unsigned seed = 1000U;
	for (const auto pair : test::pairs) {
		const auto inputs =
			test::oracle::representative_inputs(pair.source_width);
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
				test::set_source(initial, 29U,
					pair.source_width,
					inputs[(controls + rounding) %
						inputs.size()]);
				char name[224];
				std::snprintf(name, sizeof(name),
					"scalar-precision-controls source=%u "
					"destination=%u mask=%u rounding=%u",
					pair.source_width,
					pair.destination_width,
					controls, rounding);
				test::check(harness, name, pair,
					{28U, 29U}, initial);
			}
		}
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	nan_payload_default_nan_and_signaling(harness);
	source_denormal_flush_modes(harness);
	output_flush_timing(harness);
	alternative_half_source_and_destination(harness);
	every_relevant_control_combination(harness);
	harness.print_summary("arm64-simd-fcvt-precision-control");
	return 0;
}
