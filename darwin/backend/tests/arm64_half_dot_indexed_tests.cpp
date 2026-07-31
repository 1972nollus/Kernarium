// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_dot_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_half_dot_test;

namespace {

constexpr test::Registers registers{28U, 29U, 31U};

[[nodiscard]] test::arm64::CpuState indexed_state(
	unsigned first_pattern, unsigned second_pattern, unsigned seed)
{
	constexpr std::array<std::uint16_t, 16> values{
		0x0000U, 0x8000U, 0x0001U, 0x8001U,
		0x0400U, 0x8400U, 0x3c00U, 0xbc00U,
		0x3555U, 0xb555U, 0x7bffU, 0xfbffU,
		0x7c00U, 0xfc00U, 0x7e01U, 0x7c01U,
	};
	auto initial = test::state(seed);
	for (unsigned lane = 0U; lane < 4U; ++lane) {
		test::exact::write_single(
			initial.vectors[registers.destination].bytes, lane,
			0x3f00'0000U + ((first_pattern + second_pattern +
				lane) << 9U));
		test::write_half(initial.vectors[registers.first].bytes,
			lane * 2U,
			values[(first_pattern + lane * 3U) % values.size()]);
		test::write_half(initial.vectors[registers.first].bytes,
			lane * 2U + 1U,
			values[(second_pattern + lane * 5U + 1U) %
				values.size()]);
		/*
		 * Every indexed pair is distinct. A decoder that drops either H
		 * or L, or truncates the second register, fails this matrix.
		 */
		test::write_half(initial.vectors[registers.second].bytes,
			lane * 2U,
			values[(first_pattern * 7U + lane * 2U) %
				values.size()]);
		test::write_half(initial.vectors[registers.second].bytes,
			lane * 2U + 1U,
			values[(second_pattern * 11U + lane * 2U + 3U) %
				values.size()]);
	}
	return initial;
}

void every_width_index_and_relevant_operand_class()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts;
	unsigned seed = 4000U;
	for (const bool wide : {false, true})
		for (unsigned index = 0U; index < 4U; ++index)
			for (unsigned first = 0U; first < 16U; ++first)
				for (unsigned second = 0U; second < 16U;
				     ++second) {
					char name[192];
					std::snprintf(name, sizeof(name),
						"FDOT indexed wide=%u index=%u "
						"first=%u second=%u",
						static_cast<unsigned>(wide),
						index, first, second);
					test::check(harness, name,
						{true, wide, index}, registers,
						indexed_state(first, second,
							++seed),
						counts);
				}
	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-dot-indexed-behavior");
}

void second_register_bit_four_and_destructive_aliases()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts;
	unsigned seed = 7000U;
	for (const bool wide : {false, true})
		for (unsigned index = 0U; index < 4U; ++index)
			for (const auto alias : test::register_aliases)
				test::check(harness,
					"FDOT indexed full register and alias",
					{true, wide, index}, alias,
					indexed_state(index + alias.destination,
						alias.second, ++seed),
					counts);
	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-dot-indexed-alias");
}

} // namespace

int main()
{
	every_width_index_and_relevant_operand_class();
	second_register_bit_four_and_destructive_aliases();
	return 0;
}
