// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_estimate_test_support.hpp"

#include <array>
#include <cstdint>

namespace test = arm64_simd_estimate_test;

namespace {

constexpr std::array register_cases{
	test::Registers{0, 1},
	test::Registers{1, 1},
	test::Registers{1, 0},
	test::Registers{31, 30},
	test::Registers{30, 31},
	test::Registers{31, 31},
};
constexpr std::array boundary_inputs{
	std::uint32_t{0x0000'0000U},
	std::uint32_t{0x3fff'ffffU},
	std::uint32_t{0x4000'0000U},
	std::uint32_t{0x7fff'ffffU},
	std::uint32_t{0x8000'0000U},
	std::uint32_t{0xbfff'ffffU},
	std::uint32_t{0xc000'0000U},
	std::uint32_t{0xffff'ffffU},
};
constexpr std::array low_patterns{
	std::uint32_t{0},
	std::uint32_t{1},
	std::uint32_t{0x003f'ffffU},
	std::uint32_t{0x007f'ffffU},
};

void every_input_bin_and_ignored_low_bits(
	test::engine_tests::EngineHarness &harness)
{
	constexpr test::Registers registers{0, 1};
	for (const bool q : {false, true})
		for (const bool reciprocal_sqrt : {false, true})
			for (unsigned top_nine = 0; top_nine < 512; ++top_nine) {
				auto initial = test::state(top_nine);
				initial.vectors[registers.source].bytes.fill(
					std::byte{0xa5});
				const unsigned lanes = q ? 4U : 2U;
				for (unsigned lane = 0; lane < lanes; ++lane) {
					const unsigned pattern =
						q ? lane : lane * (low_patterns.size() - 1U);
					test::write_word(
						initial.vectors[registers.source].bytes, lane,
						(top_nine << 23) | low_patterns[pattern]);
				}
				harness.check_one(
					test::operation_name(reciprocal_sqrt),
					test::instruction(q, reciprocal_sqrt, false,
						registers),
					initial,
					test::expected_state(initial, q, reciprocal_sqrt,
						registers),
					test::native_requirements());
			}
}

void boundary_values_and_register_aliases(
	test::engine_tests::EngineHarness &harness)
{
	for (const bool q : {false, true})
		for (const bool reciprocal_sqrt : {false, true})
			for (const auto registers : register_cases)
				for (const auto input : boundary_inputs) {
					auto initial = test::state(input >> 23);
					for (unsigned lane = 0; lane < 4; ++lane)
						test::write_word(
							initial.vectors[registers.source].bytes,
							lane, input ^ (lane * 0x0012'3456U));
					harness.check_one(
						test::operation_name(reciprocal_sqrt),
						test::instruction(q, reciprocal_sqrt, false,
							registers),
						initial,
						test::expected_state(initial, q,
							reciprocal_sqrt, registers),
						test::native_requirements());
				}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_input_bin_and_ignored_low_bits(harness);
	boundary_values_and_register_aliases(harness);
	harness.print_summary("arm64-simd-estimate-behavior");
	return 0;
}
