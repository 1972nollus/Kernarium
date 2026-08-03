// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_lut_test_support.hpp"

namespace uml::emulator::tests::simd_lut {
namespace {

void every_register_field(EngineHarness &harness)
{
	for (const auto &form : forms) {
		for (unsigned destination = 0; destination < 32U; ++destination) {
			const auto instruction =
				encode(form, 23U, 11U, destination);
			check(harness, "simd-lut-destination-allocation",
				instruction, seeded_state(instruction));
		}
		for (unsigned table = 0; table < 32U; ++table) {
			const auto instruction =
				encode(form, 23U, table, 3U);
			check(harness, "simd-lut-table-allocation",
				instruction, seeded_state(instruction));
		}
		for (unsigned indices = 0; indices < 32U; ++indices) {
			const auto instruction =
				encode(form, indices, 11U, 3U);
			check(harness, "simd-lut-index-allocation",
				instruction, seeded_state(instruction));
		}
	}
}

void exact_control_table()
{
	unsigned allocated = 0;
	unsigned reserved = 0;
	for (unsigned op2 = 1U; op2 <= 3U; ++op2)
		for (unsigned len = 0; len < 4U; ++len)
			for (unsigned op = 0; op < 2U; ++op) {
				const auto decoded = decode(
					encode(op2, len, op, 7U, 9U, 13U));
				arm64_system_test::require(decoded.recognized);
				if (decoded.valid)
					++allocated;
				else
					++reserved;
			}
	arm64_system_test::require(allocated == 18U);
	arm64_system_test::require(reserved == 6U);
}

} // namespace
} // namespace uml::emulator::tests::simd_lut

int main()
{
	uml::emulator::tests::simd_lut::exact_control_table();
	uml::emulator::tests::EngineHarness harness;
	uml::emulator::tests::simd_lut::every_register_field(harness);
	harness.print_summary("arm64-simd-lut-allocation");
	return 0;
}
