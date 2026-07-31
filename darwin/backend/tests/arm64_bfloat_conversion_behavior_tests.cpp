// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_bfloat_conversion_test_support.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace test = arm64_bfloat_conversion_test;

namespace {

struct ExactCase final {
	std::uint32_t input;
	std::uint16_t output;
	std::uint32_t flags;
};

void require_oracle(const ExactCase &entry)
{
	const auto actual = test::convert(entry.input, 0U);
	if (actual.bits == entry.output && actual.flags == entry.flags)
		return;
	std::fprintf(stderr,
		"reduced-precision oracle mismatch input=%08x "
		"expected=%04x/%08x actual=%04x/%08x\n",
		entry.input, entry.output, entry.flags,
		actual.bits, actual.flags);
	std::abort();
}

void exact_finite_zero_special_and_denormal_values(
	test::engine_tests::EngineHarness &harness)
{
	const std::array cases{
		ExactCase{0x0000'0000U, 0x0000U, 0U},
		ExactCase{0x8000'0000U, 0x8000U, 0U},
		ExactCase{0x3f80'0000U, 0x3f80U, 0U},
		ExactCase{0xbfc0'0000U, 0xbfc0U, 0U},
		ExactCase{0x4000'0000U, 0x4000U, 0U},
		ExactCase{0x0001'0000U, 0x0001U, 0U},
		ExactCase{0x7f80'0000U, 0x7f80U, 0U},
		ExactCase{0xff80'0000U, 0xff80U, 0U},
		ExactCase{0x7fc1'2345U, 0x7fc1U, 0U},
		ExactCase{0x7f81'2345U, 0x7fc1U,
			test::floating::fpsr_invalid},
	};
	for (unsigned index = 0U; index < cases.size(); ++index) {
		const auto &entry = cases[index];
		require_oracle(entry);
		for (const auto form : test::forms) {
			auto initial = test::state(index +
				static_cast<unsigned>(form) * 100U);
			initial.fpsr = 0x0800'0090U;
			for (unsigned lane = 0U; lane < 4U; ++lane)
				test::write_single(
					initial.vectors[29].bytes, lane,
					entry.input);
			char name[160];
			std::snprintf(name, sizeof(name),
				"reduced-precision-exact form=%u index=%u",
				static_cast<unsigned>(form), index);
			test::check(harness, name, form, {28U, 29U}, initial);
		}
	}
}

void every_representative_floating_class(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 1000U;
	for (const auto form : test::forms)
		for (unsigned index = 0U;
		     index < test::representative_inputs.size(); ++index) {
			auto initial = test::state(++seed);
			for (unsigned lane = 0U; lane < 4U; ++lane)
				test::write_single(
					initial.vectors[29].bytes, lane,
					test::representative_inputs[
						(index + lane * 3U) %
						test::representative_inputs.size()]);
			char name[160];
			std::snprintf(name, sizeof(name),
				"reduced-precision-class form=%u input=%u",
				static_cast<unsigned>(form), index);
			test::check(harness, name, form, {28U, 29U}, initial);
		}
}

void deterministic_bit_pattern_samples(
	test::engine_tests::EngineHarness &harness)
{
	std::uint64_t random = 0xa409'3822'299f'31d0ULL;
	unsigned seed = 2000U;
	for (const auto form : test::forms)
		for (unsigned sample = 0U; sample < 256U; ++sample) {
			auto initial = test::state(++seed);
			for (unsigned lane = 0U; lane < 4U; ++lane) {
				random ^= random << 13U;
				random ^= random >> 7U;
				random ^= random << 17U;
				test::write_single(
					initial.vectors[29].bytes, lane,
					static_cast<std::uint32_t>(random));
			}
			char name[160];
			std::snprintf(name, sizeof(name),
				"reduced-precision-pattern form=%u sample=%u",
				static_cast<unsigned>(form), sample);
			test::check(harness, name, form, {28U, 29U}, initial);
		}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	exact_finite_zero_special_and_denormal_values(harness);
	every_representative_floating_class(harness);
	deterministic_bit_pattern_samples(harness);
	harness.print_summary("arm64-bfloat-conversion-behavior");
	return 0;
}
