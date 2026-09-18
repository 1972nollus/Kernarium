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

constexpr std::array operations{
	mte::TagMemoryOperation::store_zero_tag,
	mte::TagMemoryOperation::store_zero_two_tags,
};
constexpr std::array modes{
	mte::AddressMode::post_index,
	mte::AddressMode::signed_offset,
	mte::AddressMode::pre_index,
};

void encoding_examples()
{
	mte::require(mte::tag_memory(
		mte::TagMemoryOperation::store_zero_tag,
		mte::AddressMode::signed_offset, 0, 15, 14) ==
		0xd960'09eeU);
	mte::require(mte::tag_memory(
		mte::TagMemoryOperation::store_zero_tag,
		mte::AddressMode::post_index, 1, 31, 31) ==
		0xd960'17ffU);
	mte::require(mte::tag_memory(
		mte::TagMemoryOperation::store_zero_two_tags,
		mte::AddressMode::pre_index, 0x1fe, 13, 12) ==
		0xd9ff'edacU);
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const auto operation : operations) {
		for (const auto mode : modes) {
			for (const unsigned base : {1U, 31U}) {
				const auto built = gadgets::Builder{}.build(
					test::engine_test_code_base,
					std::array{mte::tag_memory(
						operation, mode, 0, base, 2)});
				mte::require(built.complete);
				entries.insert(built.program.words().front());
			}
		}
	}
	mte::require(entries.size() == 12);
}

} // namespace

int main()
{
	encoding_examples();
	production_specialization();
}
