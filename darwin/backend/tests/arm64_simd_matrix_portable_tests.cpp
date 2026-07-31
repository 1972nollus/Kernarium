// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_matrix_test_support.hpp"

#include <array>

namespace test = arm64_simd_matrix_test;

namespace {

void every_portable_body_matches_the_specification_oracle()
{
	test::engine_tests::EngineHarness harness{{
		.force_software_i8mm = true,
	}};
	constexpr std::array register_sets{
		test::Registers{28U, 29U, 30U},
		test::Registers{7U, 7U, 23U},
		test::Registers{3U, 3U, 3U},
	};
	for (const auto mode : test::modes)
		for (const auto registers : register_sets)
			for (unsigned pattern = 0; pattern < 4U; ++pattern) {
				auto initial = test::state(
					71U + pattern * 13U +
					static_cast<unsigned>(mode) * 29U);
				if (pattern == 1U) {
					initial.vectors[registers.left].bytes.fill(
						std::byte{0x80});
					initial.vectors[registers.right].bytes.fill(
						std::byte{0xff});
				} else if (pattern == 2U) {
					initial.vectors[registers.left].bytes.fill(
						std::byte{0xff});
					initial.vectors[registers.right].bytes.fill(
						std::byte{0x7f});
					initial.vectors[registers.destination].bytes.fill(
						std::byte{0xff});
				} else if (pattern == 3U) {
					for (unsigned byte = 0; byte < 16U; ++byte) {
						initial.vectors[registers.left].bytes[byte] =
							static_cast<std::byte>(byte * 17U + 3U);
						initial.vectors[registers.right].bytes[byte] =
							static_cast<std::byte>(byte * 29U + 5U);
					}
				}
				test::check(harness, "matrix forced portable",
					mode, registers, initial);
			}
	harness.print_summary("simd-matrix-portable");
}

} // namespace

int main()
{
	every_portable_body_matches_the_specification_oracle();
	return 0;
}
