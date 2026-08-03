// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvt_precision_test_support.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace test = arm64_simd_fcvt_precision_test;

namespace {

struct ExactCase final {
	test::Pair pair;
	std::uint64_t input;
	std::uint64_t output;
};

void require_oracle(const ExactCase &entry)
{
	const auto conversion = test::oracle::convert(entry.input,
		entry.pair.source_width, entry.pair.destination_width, 0U);
	if (conversion.bits == entry.output && conversion.flags == 0U)
		return;
	std::fprintf(stderr,
		"scalar precision oracle mismatch source=%u destination=%u "
		"input=%016llx expected=%016llx/%08x actual=%016llx/%08x\n",
		entry.pair.source_width, entry.pair.destination_width,
		static_cast<unsigned long long>(entry.input),
		static_cast<unsigned long long>(entry.output), 0U,
		static_cast<unsigned long long>(conversion.bits),
		conversion.flags);
	std::abort();
}

void exact_finite_zero_denormal_and_infinity_values(
	test::engine_tests::EngineHarness &harness)
{
	const std::array cases{
		ExactCase{{16U, 32U}, 0x0000U, 0x0000'0000U},
		ExactCase{{16U, 32U}, 0x8000U, 0x8000'0000U},
		ExactCase{{16U, 32U}, 0x3c00U, 0x3f80'0000U},
		ExactCase{{16U, 32U}, 0x0001U, 0x3380'0000U},
		ExactCase{{16U, 64U}, 0x3c00U,
			0x3ff0'0000'0000'0000ULL},
		ExactCase{{16U, 64U}, 0x0001U,
			0x3e70'0000'0000'0000ULL},
		ExactCase{{32U, 64U}, 0x3f80'0000U,
			0x3ff0'0000'0000'0000ULL},
		ExactCase{{32U, 64U}, 0x0000'0001U,
			0x36a0'0000'0000'0000ULL},
		ExactCase{{32U, 16U}, 0x3f80'0000U, 0x3c00U},
		ExactCase{{64U, 32U}, 0x3ff0'0000'0000'0000ULL,
			0x3f80'0000U},
		ExactCase{{64U, 16U}, 0x3ff0'0000'0000'0000ULL,
			0x3c00U},
	};
	for (unsigned index = 0U; index < cases.size(); ++index) {
		const auto &entry = cases[index];
		require_oracle(entry);
		auto initial = test::state(index);
		initial.fpsr = 0x0800'0090U;
		test::set_source(initial, 29U, entry.pair.source_width,
			entry.input);
		char name[160];
		std::snprintf(name, sizeof(name),
			"scalar-precision-exact index=%u source=%u destination=%u",
			index, entry.pair.source_width,
			entry.pair.destination_width);
		test::check(harness, name, entry.pair, {28U, 29U}, initial);
	}

	for (const auto pair : test::pairs)
		for (const bool negative : {false, true}) {
			auto initial = test::state(
				pair.source_width + pair.destination_width +
				static_cast<unsigned>(negative));
			initial.fpsr = 0x10U;
			test::set_source(initial, 29U, pair.source_width,
				test::infinity(pair.source_width, negative));
			char name[160];
			std::snprintf(name, sizeof(name),
				"scalar-precision-infinity source=%u destination=%u "
				"negative=%u",
				pair.source_width, pair.destination_width,
				static_cast<unsigned>(negative));
			test::check(harness, name, pair, {28U, 29U}, initial);
		}
}

void every_representative_floating_class(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 200U;
	for (const auto pair : test::pairs) {
		const auto inputs =
			test::oracle::representative_inputs(pair.source_width);
		for (unsigned index = 0U; index < inputs.size(); ++index) {
			auto initial = test::state(++seed);
			test::set_source(initial, 29U, pair.source_width,
				inputs[index]);
			char name[160];
			std::snprintf(name, sizeof(name),
				"scalar-precision-class source=%u destination=%u "
				"input=%u",
				pair.source_width, pair.destination_width, index);
			test::check(harness, name, pair, {28U, 29U}, initial);
		}
	}
}

void deterministic_bit_pattern_samples(
	test::engine_tests::EngineHarness &harness)
{
	std::uint64_t random = 0x243f'6a88'85a3'08d3ULL;
	unsigned seed = 1000U;
	for (const auto pair : test::pairs)
		for (unsigned sample = 0U; sample < 256U; ++sample) {
			random ^= random << 13U;
			random ^= random >> 7U;
			random ^= random << 17U;
			auto initial = test::state(++seed);
			initial.fpsr = (sample & 1U) != 0U ? 0x80U : 0U;
			test::set_source(initial, 29U, pair.source_width,
				random);
			char name[160];
			std::snprintf(name, sizeof(name),
				"scalar-precision-pattern source=%u destination=%u "
				"sample=%u",
				pair.source_width, pair.destination_width, sample);
			test::check(harness, name, pair, {28U, 29U}, initial);
		}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	exact_finite_zero_denormal_and_infinity_values(harness);
	every_representative_floating_class(harness);
	deterministic_bit_pattern_samples(harness);
	harness.print_summary("arm64-simd-fcvt-precision-exact");
	return 0;
}
