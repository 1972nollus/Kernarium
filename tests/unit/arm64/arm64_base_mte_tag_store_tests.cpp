// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_mte_test_support.hpp"

#include <array>
#include <cstdint>
#include <set>

namespace mte = arm64_base_mte_test;
namespace test = uml::emulator::tests;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

constexpr std::array operations{
	mte::TagMemoryOperation::store_tag,
	mte::TagMemoryOperation::store_two_tags,
};
constexpr std::array modes{
	mte::AddressMode::post_index,
	mte::AddressMode::signed_offset,
	mte::AddressMode::pre_index,
};

void encoding_examples()
{
	mte::require(mte::tag_memory(mte::TagMemoryOperation::store_tag,
		mte::AddressMode::signed_offset, 0, 6, 5) == 0xd920'08c5U);
	mte::require(mte::tag_memory(mte::TagMemoryOperation::store_tag,
		mte::AddressMode::post_index, 1, 31, 31) == 0xd920'17ffU);
	mte::require(mte::tag_memory(mte::TagMemoryOperation::store_tag,
		mte::AddressMode::pre_index, 0x1ff, 8, 7) == 0xd93f'fd07U);
	mte::require(mte::tag_memory(mte::TagMemoryOperation::store_two_tags,
		mte::AddressMode::signed_offset, 0, 10, 9) ==
		0xd9a0'0949U);
	mte::require(mte::tag_memory(mte::TagMemoryOperation::store_two_tags,
		mte::AddressMode::post_index, 2, 11, 31) ==
		0xd9a0'257fU);
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const auto operation : operations) {
		for (const auto mode : modes) {
			for (const unsigned base : {1U, 31U}) {
				for (const unsigned source : {2U, 31U}) {
					const auto built = gadgets::Builder{}.build(
						test::engine_test_code_base,
						std::array{mte::tag_memory(
							operation, mode, 0, base,
							source)});
					mte::require(built.complete &&
						built.program.instruction_count() == 1);
					entries.insert(
						built.program.words().front());
				}
			}
		}
	}
	/* The source is a direct CpuState offset rather than a gadget choice. */
	mte::require(entries.size() == 12);
}

} // namespace

int main()
{
	encoding_examples();
	production_specialization();
}
