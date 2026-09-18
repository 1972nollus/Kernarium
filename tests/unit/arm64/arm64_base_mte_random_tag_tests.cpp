// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_mte_test_support.hpp"

#include <array>
#include <cstdint>
#include <set>

namespace mte = arm64_base_mte_test;
namespace test = uml::emulator::tests;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

void encoding_examples()
{
	mte::require(mte::irg(0, 1, 2) == 0x9ac2'1020U);
	mte::require(mte::irg(31, 31, 31) == 0x9adf'13ffU);
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const bool source_sp : {false, true}) {
		for (const bool mask_zero : {false, true}) {
			for (const bool destination_sp : {false, true}) {
				const auto built = gadgets::Builder{}.build(
					test::engine_test_code_base,
					std::array{mte::irg(
						destination_sp ? 31U : 3U,
						source_sp ? 31U : 1U,
						mask_zero ? 31U : 2U)});
				mte::require(built.complete &&
					built.program.instruction_count() == 1 &&
					built.program.words().size() == 5);
				entries.insert(built.program.words().front());
			}
		}
	}
	mte::require(entries.size() == 8);
}

} // namespace

int main()
{
	encoding_examples();
	production_specialization();
}
