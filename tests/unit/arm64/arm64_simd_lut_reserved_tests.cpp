// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_lut_test_support.hpp"

namespace uml::emulator::tests::simd_lut {
namespace {

void reserved_control_cells(EngineHarness &harness)
{
	for (unsigned op2 = 1U; op2 <= 3U; ++op2)
		for (unsigned len = 0; len < 4U; ++len)
			for (unsigned op = 0; op < 2U; ++op) {
				const auto representative =
					encode(op2, len, op, 23U, 11U, 3U);
				if (decode(representative).valid)
					continue;
				for (unsigned destination = 0;
				     destination < 32U; ++destination) {
					const auto instruction = encode(op2,
						len, op, 23U, 11U,
						destination);
					check_reserved(harness,
						"simd-lut-reserved-destination",
						instruction,
						seeded_state(instruction));
				}
				for (unsigned table = 0; table < 32U;
				     ++table) {
					const auto instruction = encode(op2,
						len, op, 23U, table, 3U);
					check_reserved(harness,
						"simd-lut-reserved-table",
						instruction,
						seeded_state(instruction));
				}
				for (unsigned indices = 0; indices < 32U;
				     ++indices) {
					const auto instruction = encode(op2,
						len, op, indices, 11U, 3U);
					check_reserved(harness,
						"simd-lut-reserved-index",
						instruction,
						seeded_state(instruction));
				}
			}
}

} // namespace
} // namespace uml::emulator::tests::simd_lut

int main()
{
	uml::emulator::tests::EngineHarness harness;
	uml::emulator::tests::simd_lut::reserved_control_cells(harness);
	harness.print_summary("arm64-simd-lut-reserved");
	return 0;
}
