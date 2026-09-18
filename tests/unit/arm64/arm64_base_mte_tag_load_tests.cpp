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
	mte::require(mte::load_tag(0, 1, 0) == 0xd960'0020U);
	mte::require(mte::load_tag(0x100, 31, 2) == 0xd970'03e2U);
	mte::require(mte::load_tag(0x1ff, 30, 31) == 0xd97f'f3dfU);
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const unsigned base : {1U, 31U}) {
		for (const unsigned destination : {2U, 31U}) {
			const auto built = gadgets::Builder{}.build(
				test::engine_test_code_base,
				std::array{mte::load_tag(0, base, destination)});
			mte::require(built.complete &&
				built.program.instruction_count() == 1);
			entries.insert(built.program.words().front());
		}
	}
	mte::require(entries.size() == 4);
}

} // namespace

int main()
{
	encoding_examples();
	production_specialization();
}
