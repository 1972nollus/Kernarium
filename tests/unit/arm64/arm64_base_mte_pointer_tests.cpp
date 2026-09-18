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
	mte::require(mte::gmi(0, 1, 2) == 0x9ac2'1420U);
	mte::require(mte::gmi(30, 31, 31) == 0x9adf'17feU);
	mte::require(mte::subp(false, 0, 1, 2) == 0x9ac2'0020U);
	mte::require(mte::subp(false, 30, 31, 31) == 0x9adf'03feU);
	mte::require(mte::subp(true, 0, 1, 2) == 0xbac2'0020U);
	mte::require(mte::subp(true, 31, 1, 2) == 0xbac2'003fU);
	mte::require(mte::subp(true, 31, 31, 31) == 0xbadf'03ffU);
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (unsigned operation = 0; operation < 3; ++operation) {
		for (unsigned mode = 0; mode < 8; ++mode) {
			const unsigned first = (mode & 4U) != 0 ? 31U : 1U;
			const unsigned second = (mode & 2U) != 0 ? 31U : 2U;
			const unsigned destination = (mode & 1U) != 0 ? 31U : 3U;
			const auto instruction = operation == 0 ?
				mte::gmi(destination, first, second) :
				mte::subp(operation == 2, destination, first, second);
			const auto built = gadgets::Builder{}.build(
				test::engine_test_code_base, std::array{instruction});
			mte::require(built.complete &&
				built.program.instruction_count() == 1 &&
				built.program.words().size() == 4);
			entries.insert(built.program.words().front());
		}
	}
	mte::require(entries.size() == 24);
}

} // namespace

int main()
{
	encoding_examples();
	production_specialization();
}
