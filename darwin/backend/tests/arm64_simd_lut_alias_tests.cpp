// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_lut_test_support.hpp"

#include <array>

namespace uml::backend::tests::simd_lut {
namespace {

struct Registers final {
	unsigned destination;
	unsigned table;
	unsigned indices;
};

constexpr std::array aliases{
	Registers{3U, 11U, 23U},
	Registers{11U, 11U, 23U},
	Registers{23U, 11U, 23U},
	Registers{7U, 19U, 19U},
	Registers{5U, 5U, 5U},
	Registers{12U, 11U, 23U},
	Registers{12U, 11U, 12U},
	Registers{0U, 31U, 17U},
};

void destructive_aliases(EngineHarness &harness)
{
	for (const auto &form : forms)
		for (unsigned alias_index = 0; alias_index < aliases.size();
		     ++alias_index) {
			const auto registers = aliases[alias_index];
			const auto instruction = encode(form,
				registers.indices, registers.table,
				registers.destination);
			auto initial = seeded_state(
				instruction ^ (alias_index * 0x101U));
			const unsigned elements = 16U / form.element_bytes;
			const unsigned mask =
				(1U << form.index_bits) - 1U;
			for (unsigned element = 0; element < elements;
			     ++element)
				write_index(initial.vectors[registers.indices],
					form, element,
					(element + alias_index * 3U) & mask);
			check(harness, "simd-lut-register-alias",
				instruction, initial);
		}
}

} // namespace
} // namespace uml::backend::tests::simd_lut

int main()
{
	uml::backend::tests::EngineHarness harness;
	uml::backend::tests::simd_lut::destructive_aliases(harness);
	harness.print_summary("arm64-simd-lut-alias");
	return 0;
}
