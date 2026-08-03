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

void all_register_fields(test::EngineHarness &harness)
{
	const auto initial = mte::initial_state();
	for (const bool subtract : {false, true}) {
		for (unsigned destination = 0; destination < 32; ++destination) {
			for (unsigned source = 0; source < 32; ++source) {
				const unsigned immediate =
					(source * 5U + destination) & 63U;
				const unsigned tag_offset =
					(source + destination * 3U) & 15U;
				const mte::CaseName name{"%s-d%u-n%u-i%u-t%u",
					subtract ? "subg" : "addg", destination,
					source, immediate, tag_offset};
				mte::check_one(harness, name,
					mte::add_sub_tag(subtract, destination, source,
						immediate, tag_offset),
					initial, mte::expected_add_sub_tag(initial,
						subtract, destination, source,
						immediate, tag_offset));
			}
		}
	}
}

void all_immediates_tags_and_start_tags(test::EngineHarness &harness)
{
	constexpr std::array excludes{
		std::uint16_t{0}, std::uint16_t{1},
		std::uint16_t{0x8000}, std::uint16_t{0xaaaa},
		std::uint16_t{0x5555}, std::uint16_t{0xfffe},
		std::uint16_t{0x7fff}, std::uint16_t{0xffff}};
	for (const auto exclude : excludes) {
		for (unsigned start = 0; start < 16; ++start) {
			auto initial = mte::initial_state();
			initial.set_mte_configuration(true, exclude);
			initial.registers[1] =
				(static_cast<std::uint64_t>(start) << 56) |
				0x00ff'ffff'ffff'fc10ULL;
			for (unsigned immediate = 0; immediate < 64; ++immediate) {
				for (unsigned tag_offset = 0;
				     tag_offset < 16; ++tag_offset) {
					for (const bool subtract : {false, true}) {
						const mte::CaseName name{
							"%s-e%x-s%u-i%u-t%u",
							subtract ? "subg" : "addg",
							static_cast<unsigned>(exclude),
							start, immediate, tag_offset};
						mte::check_one(harness, name,
							mte::add_sub_tag(subtract, 2, 1,
								immediate, tag_offset),
							initial,
							mte::expected_add_sub_tag(initial,
								subtract, 2, 1, immediate,
								tag_offset));
					}
				}
			}
		}
	}
}

void every_exclusion_mask(test::EngineHarness &harness)
{
	for (unsigned raw = 0; raw <= 0xffffU; ++raw) {
		auto initial = mte::initial_state();
		const auto exclude = static_cast<std::uint16_t>(raw);
		initial.set_mte_configuration(true, exclude);
		const unsigned start = raw & 15U;
		const unsigned tag_offset = (raw >> 4) & 15U;
		const unsigned immediate = (raw >> 10) & 63U;
		initial.registers[1] =
			(static_cast<std::uint64_t>(start) << 56) |
			0x0000'0000'0000'0010ULL;
		for (const bool subtract : {false, true}) {
			const mte::CaseName name{"%s-exclude-%04x",
				subtract ? "subg" : "addg", raw};
			mte::check_one(harness, name,
				mte::add_sub_tag(subtract, 2, 1,
					immediate, tag_offset),
				initial, mte::expected_add_sub_tag(initial,
					subtract, 2, 1, immediate, tag_offset));
		}
	}
}

void disabled_and_arithmetic_wrap(test::EngineHarness &harness)
{
	for (unsigned start = 0; start < 16; ++start) {
		auto initial = mte::initial_state();
		initial.set_mte_configuration(false, 0x8421U);
		initial.sp = (static_cast<std::uint64_t>(start) << 56) |
			0x00ff'ffff'ffff'fff0ULL;
		for (unsigned tag_offset = 0; tag_offset < 16; ++tag_offset) {
			for (const bool subtract : {false, true}) {
				for (const unsigned destination : {2U, 31U}) {
					const mte::CaseName name{
						"%s-disabled-s%u-t%u-d%u",
						subtract ? "subg" : "addg", start,
						tag_offset, destination};
					mte::check_one(harness, name,
						mte::add_sub_tag(subtract, destination,
							31, 63, tag_offset),
						initial,
						mte::expected_add_sub_tag(initial,
							subtract, destination, 31, 63,
							tag_offset));
				}
			}
		}
	}
}

void reserved_allocations(test::EngineHarness &harness)
{
	const auto initial = mte::initial_state();
	/* sf==0 and S==1 are explicit UNALLOCATED decode partitions. The op3
	 * encodings are fixed RES0 fields on ADDG/SUBG instruction pages.
	 */
	constexpr std::array invalid{
		0x1180'0020U,
		0x5180'0020U,
		0xb180'0020U,
		0xf180'0020U,
		0x9180'4020U,
		0x9180'8020U,
		0xd180'4020U,
		0xd180'8020U,
	};
	for (unsigned index = 0; index < invalid.size(); ++index) {
		const mte::CaseName name{"add-sub-tag-reserved-%u", index};
		harness.check_illegal(name, invalid[index], initial, mte::native_mte);
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
	test::EngineHarness harness;
	all_register_fields(harness);
	all_immediates_tags_and_start_tags(harness);
	every_exclusion_mask(harness);
	disabled_and_arithmetic_wrap(harness);
	reserved_allocations(harness);
	production_specialization();
	harness.print_summary("arm64-base-mte-add-sub-tag");
}
