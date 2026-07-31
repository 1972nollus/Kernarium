// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvt_precision_test_support.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace test = arm64_simd_fcvt_precision_test;

namespace {

struct RoundingCase final {
	test::Pair pair;
	std::uint64_t input;
	std::array<std::uint64_t, 4> outputs;
};

void require_oracle(test::Pair pair, std::uint64_t input,
	std::uint32_t fpcr, std::uint64_t bits, std::uint32_t flags)
{
	const auto actual = test::oracle::convert(input, pair.source_width,
		pair.destination_width, fpcr);
	if (actual.bits == bits && actual.flags == flags)
		return;
	std::fprintf(stderr,
		"scalar precision rounding oracle mismatch source=%u "
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
	initial.fpsr = 0x0800'0080U;
	test::set_source(initial, 29U, pair.source_width, input);
	test::check(harness, name, pair, {28U, 29U}, initial);
}

void all_fpcr_rounding_modes_and_ties(
	test::engine_tests::EngineHarness &harness)
{
	const std::array cases{
		RoundingCase{{32U, 16U}, 0x3f80'1000U,
			{0x3c00U, 0x3c01U, 0x3c00U, 0x3c00U}},
		RoundingCase{{32U, 16U}, 0xbf80'1000U,
			{0xbc00U, 0xbc00U, 0xbc01U, 0xbc00U}},
		RoundingCase{{64U, 32U}, 0x3ff0'0000'1000'0000ULL,
			{0x3f80'0000U, 0x3f80'0001U, 0x3f80'0000U,
				0x3f80'0000U}},
		RoundingCase{{64U, 32U}, 0xbff0'0000'1000'0000ULL,
			{0xbf80'0000U, 0xbf80'0000U, 0xbf80'0001U,
				0xbf80'0000U}},
		RoundingCase{{64U, 32U}, 0x3ff0'0000'3000'0000ULL,
			{0x3f80'0002U, 0x3f80'0002U, 0x3f80'0001U,
				0x3f80'0001U}},
	};
	for (unsigned index = 0U; index < cases.size(); ++index)
		for (unsigned mode = 0U; mode < 4U; ++mode) {
			const auto &entry = cases[index];
			char name[160];
			std::snprintf(name, sizeof(name),
				"scalar-precision-rounding index=%u mode=%u",
				index, mode);
			run_explicit(harness, name, entry.pair, entry.input,
				mode << 22U, entry.outputs[mode],
				test::oracle::fpsr_inexact,
				index * 4U + mode);
		}
}

void overflow_and_underflow_flags(
	test::engine_tests::EngineHarness &harness)
{
	for (unsigned mode = 0U; mode < 4U; ++mode) {
		const bool to_infinity = mode == 0U || mode == 1U;
		char overflow_name[128];
		std::snprintf(overflow_name, sizeof(overflow_name),
			"scalar-precision-overflow mode=%u", mode);
		run_explicit(harness, overflow_name, {64U, 32U},
			0x7fe0'0000'0000'0000ULL, mode << 22U,
			to_infinity ? 0x7f80'0000U : 0x7f7f'ffffU,
			test::oracle::fpsr_overflow |
				test::oracle::fpsr_inexact,
			100U + mode);

		char underflow_name[128];
		std::snprintf(underflow_name, sizeof(underflow_name),
			"scalar-precision-underflow mode=%u", mode);
		run_explicit(harness, underflow_name, {64U, 32U},
			0x3690'0000'0000'0000ULL, mode << 22U,
			mode == 1U ? 1U : 0U,
			test::oracle::fpsr_underflow |
				test::oracle::fpsr_inexact,
			110U + mode);
	}

	run_explicit(harness, "scalar-precision-exact-tiny", {64U, 32U},
		0x36a0'0000'0000'0000ULL, 0U, 1U, 0U, 120U);

	for (const bool ah : {false, true}) {
		const auto flags = test::oracle::fpsr_inexact |
			(ah ? 0U : test::oracle::fpsr_underflow);
		run_explicit(harness,
			ah ? "scalar-precision-boundary-ah" :
				"scalar-precision-boundary-standard",
			{64U, 32U}, 0x380f'ffff'f000'0000ULL,
			ah ? test::oracle::fpcr_ah : 0U,
			0x0080'0000U, flags, 130U + ah);
	}
}

void deterministic_samples_in_every_rounding_mode(
	test::engine_tests::EngineHarness &harness)
{
	std::uint64_t random = 0x1319'8a2e'0370'7344ULL;
	unsigned seed = 1000U;
	for (const auto pair : test::pairs)
		for (unsigned mode = 0U; mode < 4U; ++mode)
			for (unsigned sample = 0U; sample < 128U; ++sample) {
				random ^= random << 13U;
				random ^= random >> 7U;
				random ^= random << 17U;
				auto initial = test::state(++seed);
				initial.fpcr = mode << 22U;
				initial.fpsr =
					(sample & 1U) != 0U ? 0x80U : 0U;
				test::set_source(initial, 29U,
					pair.source_width, random);
				char name[192];
				std::snprintf(name, sizeof(name),
					"scalar-precision-round-pattern source=%u "
					"destination=%u mode=%u sample=%u",
					pair.source_width,
					pair.destination_width, mode, sample);
				test::check(harness, name, pair,
					{28U, 29U}, initial);
			}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	all_fpcr_rounding_modes_and_ties(harness);
	overflow_and_underflow_flags(harness);
	deterministic_samples_in_every_rounding_mode(harness);
	harness.print_summary("arm64-simd-fcvt-precision-rounding");
	return 0;
}
