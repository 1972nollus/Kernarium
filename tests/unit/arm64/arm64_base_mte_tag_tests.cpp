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
	mte::require(mte::add_sub_tag(false, 0, 1, 0, 0) ==
		0x9180'0020U);
	mte::require(mte::add_sub_tag(false, 30, 31, 63, 15) ==
		0x91bf'3ffeU);
	mte::require(mte::add_sub_tag(false, 31, 31, 1, 1) ==
		0x9181'07ffU);
	mte::require(mte::add_sub_tag(true, 0, 1, 0, 0) ==
		0xd180'0020U);
	mte::require(mte::add_sub_tag(true, 30, 31, 63, 15) ==
		0xd1bf'3ffeU);
	mte::require(mte::add_sub_tag(true, 31, 31, 1, 1) ==
		0xd181'07ffU);
}

void derived_table_matches_independent_oracle()
{
	constexpr std::array excludes{
		std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0x8000},
		std::uint16_t{0x8421}, std::uint16_t{0xaaaa},
		std::uint16_t{0x5555}, std::uint16_t{0xfffe},
		std::uint16_t{0x7fff}, std::uint16_t{0xffff}};
	for (const bool enabled : {false, true}) {
		for (const auto exclude : excludes) {
			auto state = mte::initial_state();
			state.set_mte_configuration(enabled, exclude);
			for (unsigned start = 0; start < 16; ++start)
				for (unsigned offset = 0; offset < 16; ++offset)
					mte::require(
						state.mte_tag_choices[start * 16 + offset] ==
						mte::choose_nonexcluded_tag_or_zero(
							enabled, exclude, start, offset));
		}
	}
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const bool subtract : {false, true}) {
		for (const bool source_sp : {false, true}) {
			for (const bool destination_sp : {false, true}) {
				const auto instruction = mte::add_sub_tag(subtract,
					destination_sp ? 31U : 2U,
					source_sp ? 31U : 1U, 63, 15);
				const auto built = gadgets::Builder{}.build(
					test::engine_test_code_base,
					std::array{instruction});
				mte::require(built.complete &&
					built.program.instruction_count() == 1 &&
					built.program.words().size() == 4);
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
	derived_table_matches_independent_oracle();
	production_specialization();
}
