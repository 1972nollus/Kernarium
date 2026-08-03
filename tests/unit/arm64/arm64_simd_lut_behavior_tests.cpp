// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_lut_test_support.hpp"

namespace uml::emulator::tests::simd_lut {
namespace {

void deterministic_patterns(EngineHarness &harness)
{
	constexpr unsigned destination = 13U;
	constexpr unsigned table = 5U;
	constexpr unsigned indices = 21U;
	for (const auto &form : forms)
		for (unsigned pattern = 0; pattern < 256U; ++pattern) {
			const auto instruction =
				encode(form, indices, table, destination);
			auto initial = seeded_state(
				instruction ^ (pattern * 0x9e37U));
			for (unsigned byte = 0; byte < 16U; ++byte) {
				initial.vectors[table].bytes[byte] =
					static_cast<std::byte>(
						0x11U + byte * 13U + pattern);
				initial.vectors[table + 1U].bytes[byte] =
					static_cast<std::byte>(
						0xd7U - byte * 7U + pattern * 3U);
			}
			initial.vectors[indices].bytes.fill(std::byte{});
			const unsigned elements = 16U / form.element_bytes;
			const unsigned mask =
				(1U << form.index_bits) - 1U;
			for (unsigned element = 0; element < elements;
			     ++element)
				write_index(initial.vectors[indices], form,
					element,
					(pattern + element * 3U +
						form.part * 5U) &
						mask);
			check(harness, "simd-lut-pattern-behavior",
				instruction, initial);
		}
}

} // namespace
} // namespace uml::emulator::tests::simd_lut

int main()
{
	uml::emulator::tests::EngineHarness harness;
	uml::emulator::tests::simd_lut::deterministic_patterns(harness);
	harness.print_summary("arm64-simd-lut-behavior");
	return 0;
}
