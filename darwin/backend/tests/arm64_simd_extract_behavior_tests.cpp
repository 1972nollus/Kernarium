// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_extract_test_support.hpp"

#include <array>

namespace test = arm64_simd_extract_test;

namespace {

constexpr std::array register_cases{
	test::Registers{0, 1, 2}, test::Registers{1, 1, 2},
	test::Registers{2, 1, 2}, test::Registers{1, 1, 1},
	test::Registers{31, 30, 29},
};

void every_offset_width_and_alias()
{
	test::engine_tests::EngineHarness harness;
	for (const bool q : {false, true}) {
		const unsigned bytes = q ? 16U : 8U;
		for (unsigned offset = 0; offset < bytes; ++offset)
			for (const auto registers : register_cases)
				for (unsigned seed = 0; seed < 4; ++seed) {
					auto initial = test::state(seed + offset * 11U);
					const auto instruction =
						test::instruction(q, offset, registers);
					harness.check_one("extract behavior",
						instruction, initial,
						test::expected_state(initial, q, offset,
							registers),
						test::native_requirements());
				}
	}
	harness.print_summary("simd-extract-behavior");
}

} // namespace

int main()
{
	every_offset_width_and_alias();
	return 0;
}
