// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_integer_to_floating_test_support.hpp"

namespace test = arm64_integer_to_floating_test;

namespace {

void check_edges(test::engine_tests::EngineHarness &harness,
	const char *name, std::uint32_t word, unsigned &seed)
{
	for (const auto registers : test::register_edges) {
		const auto encoded = (word & ~0x3ffU) |
			(registers.source << 5) | registers.destination;
		harness.check_illegal(name, encoded, test::state(++seed));
	}
}

[[nodiscard]] constexpr std::uint32_t scalar_simd_word(
	bool unsigned_source, bool sf, unsigned type) noexcept
{
	return 0x1e20'0000U | (sf ? 0x8000'0000U : 0U) |
		((type & 3U) << 22) | (3U << 19) |
		((unsigned_source ? 5U : 4U) << 16);
}

void cross_width_reserved_combinations(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	for (const bool unsigned_source : {false, true}) {
		check_edges(harness,
			"integer-to-floating-cross-reserved-type",
			scalar_simd_word(unsigned_source, false, 2U), seed);
		check_edges(harness,
			"integer-to-floating-cross-reserved-type",
			scalar_simd_word(unsigned_source, true, 2U), seed);
		check_edges(harness,
			"integer-to-floating-cross-reserved-same-width",
			scalar_simd_word(unsigned_source, false, 0U), seed);
		check_edges(harness,
			"integer-to-floating-cross-reserved-same-width",
			scalar_simd_word(unsigned_source, true, 1U), seed);
	}
}

[[nodiscard]] constexpr std::uint32_t scalar_gpr_word(
	bool fixed, bool unsigned_source, bool sf, unsigned type,
	unsigned scale) noexcept
{
	return (fixed ? 0x1e00'0000U : 0x1e20'0000U) |
		(sf ? 0x8000'0000U : 0U) | ((type & 3U) << 22) |
		((unsigned_source ? 3U : 2U) << 16) |
		((scale & 63U) << 10);
}

void scalar_gpr_reserved_combinations(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 1'000U;
	for (const bool unsigned_source : {false, true})
		for (const bool sf : {false, true}) {
			check_edges(harness,
				"integer-to-floating-gpr-fixed-reserved-type",
				scalar_gpr_word(true, unsigned_source, sf, 2U, 63U),
				seed);
			check_edges(harness,
				"integer-to-floating-gpr-integer-reserved-type",
				scalar_gpr_word(false, unsigned_source, sf, 2U, 0U),
				seed);
		}

	for (const bool unsigned_source : {false, true})
		for (const unsigned type : {0U, 1U, 3U})
			for (unsigned scale = 0U; scale < 32U; ++scale)
				check_edges(harness,
					"integer-to-floating-gpr-fixed-reserved-32-bit-scale",
					scalar_gpr_word(true, unsigned_source,
						false, type, scale),
					seed);
}

[[nodiscard]] constexpr std::uint32_t simd_fixed_word(
	bool scalar, bool unsigned_source, bool q,
	unsigned immediate) noexcept
{
	return (scalar ? 0x5f00'e400U : 0x0f00'e400U) |
		(unsigned_source ? 0x2000'0000U : 0U) |
		(q ? 0x4000'0000U : 0U) |
		((immediate & 127U) << 16);
}

void simd_fixed_reserved_combinations(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 10'000U;
	for (const bool unsigned_source : {false, true}) {
		for (unsigned immediate = 0U; immediate < 16U; ++immediate)
			check_edges(harness,
				"integer-to-floating-simd-fixed-scalar-reserved-immh",
				simd_fixed_word(
					true, unsigned_source, false, immediate),
				seed);
		for (unsigned immediate = 8U; immediate < 16U; ++immediate)
			for (const bool q : {false, true})
				check_edges(harness,
					"integer-to-floating-simd-fixed-vector-reserved-immh",
					simd_fixed_word(false, unsigned_source,
						q, immediate),
					seed);
		for (unsigned immediate = 64U; immediate < 128U; ++immediate)
			check_edges(harness,
				"integer-to-floating-simd-fixed-vector-reserved-1d",
				simd_fixed_word(
					false, unsigned_source, false, immediate),
				seed);
	}
	/*
	 * Vector immh=0000 words are deliberately absent: the PDF assigns them
	 * to the Advanced SIMD modified-immediate allocation, so they are not
	 * reserved encodings of this instruction.
	 */
}

void simd_integer_reserved_double_arrangement(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 20'000U;
	for (const bool unsigned_source : {false, true})
		check_edges(harness,
			"integer-to-floating-simd-integer-reserved-1d",
			0x0e61'd800U |
				(unsigned_source ? 0x2000'0000U : 0U),
			seed);
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	cross_width_reserved_combinations(harness);
	scalar_gpr_reserved_combinations(harness);
	simd_fixed_reserved_combinations(harness);
	simd_integer_reserved_double_arrangement(harness);
	harness.print_summary("arm64-integer-to-floating-reserved");
	return 0;
}
