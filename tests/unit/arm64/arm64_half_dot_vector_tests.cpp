// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_dot_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_half_dot_test;

namespace {

constexpr test::Registers registers{28U, 29U, 30U};

[[nodiscard]] test::arm64::CpuState vector_state(
	unsigned first_pattern, unsigned second_pattern, unsigned seed)
{
	constexpr std::array<std::uint16_t, 20> values{
		0x0000U, 0x8000U, 0x0001U, 0x8001U,
		0x03ffU, 0x83ffU, 0x0400U, 0x8400U,
		0x3c00U, 0xbc00U, 0x3555U, 0xb555U,
		0x7bffU, 0xfbffU, 0x7c00U, 0xfc00U,
		0x7e01U, 0xfe02U, 0x7c01U, 0xfc02U,
	};
	constexpr std::array<std::uint32_t, 12> addends{
		0x0000'0000U, 0x8000'0000U, 0x0000'0001U,
		0x8000'0001U, 0x3f80'0000U, 0xbf80'0000U,
		0x3f80'0001U, 0x7f7f'ffffU, 0x7f80'0000U,
		0xff80'0000U, 0x7fc0'1234U, 0x7f80'1234U,
	};
	auto initial = test::state(seed);
	for (unsigned lane = 0U; lane < 4U; ++lane) {
		test::exact::write_single(
			initial.vectors[registers.destination].bytes, lane,
			addends[(first_pattern + second_pattern + lane) %
				addends.size()]);
		test::write_half(initial.vectors[registers.first].bytes,
			lane * 2U,
			values[(first_pattern + lane * 3U) % values.size()]);
		test::write_half(initial.vectors[registers.first].bytes,
			lane * 2U + 1U,
			values[(second_pattern + lane * 5U + 1U) %
				values.size()]);
		test::write_half(initial.vectors[registers.second].bytes,
			lane * 2U,
			values[(first_pattern * 7U + second_pattern + lane) %
				values.size()]);
		test::write_half(initial.vectors[registers.second].bytes,
			lane * 2U + 1U,
			values[(first_pattern + second_pattern * 11U +
				lane + 2U) % values.size()]);
	}
	return initial;
}

void exact_fused_sum_anchor()
{
	const auto result = test::dot_add(0U, 0x3c01U, 0x3c00U,
		0x3c01U, 0xbc00U, 0U);
	test::require(result.bits == 0x3b00'1000U,
		"FDOT exact fused sum anchor mismatch");
	test::require(result.flags == 0U,
		"FDOT exact fused sum raised an exception");
}

void every_vector_width_and_relevant_operand_class()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts;
	unsigned seed = 2000U;
	for (const bool wide : {false, true})
		for (unsigned first = 0U; first < 20U; ++first)
			for (unsigned second = 0U; second < 20U; ++second) {
				char name[160];
				std::snprintf(name, sizeof(name),
					"FDOT vector wide=%u first=%u second=%u",
					static_cast<unsigned>(wide), first, second);
				test::check(harness, name,
					{false, wide, 0U}, registers,
					vector_state(first, second, ++seed), counts);
			}
	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-dot-vector-behavior");
}

void inactive_upper_half_is_zeroed()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts;
	auto initial = vector_state(8U, 9U, 3000U);
	for (unsigned byte = 8U; byte < 16U; ++byte)
		initial.vectors[registers.destination].bytes[byte] =
			std::byte{0xff};
	test::check(harness, "FDOT vector 2S clears upper half",
		{false, false, 0U}, registers, initial, counts);
	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-dot-vector-zeroing");
}

} // namespace

int main()
{
	exact_fused_sum_anchor();
	every_vector_width_and_relevant_operand_class();
	inactive_upper_half_is_zeroed();
	return 0;
}
