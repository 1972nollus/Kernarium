// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_mte_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>

namespace mte = arm64_base_mte_test;
namespace test = uml::emulator::tests;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

constexpr std::array modes{
	mte::AddressMode::post_index,
	mte::AddressMode::signed_offset,
	mte::AddressMode::pre_index,
};

void encoding_examples()
{
	mte::require(mte::store_pair_tag(
		mte::AddressMode::signed_offset, 0, 22, 20, 21) ==
		0x6900'56d4U);
	mte::require(mte::store_pair_tag(
		mte::AddressMode::post_index, 1, 31, 23, 24) ==
		0x6880'e3f7U);
	mte::require(mte::store_pair_tag(
		mte::AddressMode::pre_index, 0x7f, 27, 25, 26) ==
		0x69bf'eb79U);
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const auto mode : modes) {
		for (const unsigned base : {1U, 31U}) {
			for (const unsigned first : {2U, 31U}) {
				for (const unsigned second : {3U, 31U}) {
					const auto built = gadgets::Builder{}.build(
						test::engine_test_code_base,
						std::array{mte::store_pair_tag(
							mode, 0, base, first,
							second)});
					mte::require(built.complete);
					entries.insert(
						built.program.words().front());
				}
			}
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
