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

void check_valid(test::EngineHarness &harness, mte::AddressMode mode,
	unsigned imm7, unsigned base, unsigned first, unsigned second,
	std::string_view name)
{
	mte::Page initial_memory;
	mte::fill_pattern(initial_memory,
		static_cast<unsigned>(mode) * 23U + imm7);
	auto expected_memory = initial_memory;
	const auto target_offset = (mte::page_size / 2) & ~std::size_t{15};
	const auto target = mte::data_base + target_offset;
	auto initial = mte::initial_state();
	if (first != 31)
		initial.registers[first] =
			0x1122'3344'5566'7700ULL + first;
	if (second != 31)
		initial.registers[second] =
			0x8899'aabb'ccdd'ee00ULL + second;
	const auto base_address =
		(mode == mte::AddressMode::post_index ?
		 target : target - mte::offset7(imm7)) |
		(static_cast<std::uint64_t>(
			(base * 7U + first + second + 4U) & 15U) << 56);
	mte::write_register(initial, base, base_address, true);
	const auto first_value = mte::read_register(initial, first, false);
	const auto second_value = mte::read_register(initial, second, false);
	mte::store_u64(expected_memory, target_offset, first_value);
	mte::store_u64(expected_memory, target_offset + 8, second_value);
	const auto effective = mode == mte::AddressMode::post_index ?
		base_address : base_address + mte::offset7(imm7);
	const auto tag = static_cast<std::uint8_t>(mte::logical_tag(effective));
	auto expected = initial;
	if (mode == mte::AddressMode::post_index ||
	    mode == mte::AddressMode::pre_index) {
		mte::write_register(expected, base,
			base_address + mte::offset7(imm7), true);
	}
	const std::array initial_tags{
		test::AllocationTag{.offset = target_offset, .value = 1},
	};
	const std::array expected_tags{
		test::AllocationTag{.offset = target_offset, .value = tag},
	};
	const std::array regions{mte::memory_region(
		initial_memory, expected_memory, initial_tags, expected_tags)};
	harness.check({
		.name = name,
		.instruction =
			mte::store_pair_tag(mode, imm7, base, first, second),
		.initial = initial,
		.expected = expected,
		.native = mte::native_mte,
		.memory = regions,
	});
}

void all_register_fields(test::EngineHarness &harness)
{
	for (const auto mode : modes) {
		for (unsigned first = 0; first < 32; ++first) {
			for (unsigned second = 0; second < 32; ++second) {
				const unsigned imm7 =
					(first * 17U + second * 5U) & 0x7fU;
				const mte::CaseName name{
					"stgp-%u-b5-t%u-t2%u-i%u",
					static_cast<unsigned>(mode), first,
					second, imm7};
				check_valid(
					harness, mode, imm7, 5, first, second, name);
			}
		}
		for (unsigned base = 0; base < 32; ++base) {
			for (const unsigned first : {0U, 7U, 31U}) {
				for (const unsigned second : {1U, 9U, 31U}) {
					const unsigned imm7 = (base * 11U +
						first * 3U + second) & 0x7fU;
					const mte::CaseName name{
						"stgp-%u-b%u-t%u-t2%u-i%u",
						static_cast<unsigned>(mode), base,
						first, second, imm7};
					check_valid(harness, mode, imm7,
						base, first, second, name);
				}
			}
		}
	}
}

void all_immediates(test::EngineHarness &harness)
{
	for (const auto mode : modes) {
		for (unsigned imm7 = 0; imm7 < 128; ++imm7) {
			const mte::CaseName name{"stgp-%u-imm%u",
				static_cast<unsigned>(mode), imm7};
			check_valid(harness, mode, imm7, 6, 7, 8, name);
		}
	}
}

void alignment_mapping_and_reserved_faults(test::EngineHarness &harness)
{
	for (const auto mode : modes) {
		auto initial = mte::initial_state();
		const auto effective = mte::data_base + 0x301;
		initial.registers[1] =
			mode == mte::AddressMode::post_index ?
				effective : effective - 16;
		const mte::CaseName name{"stgp-%u-unaligned",
			static_cast<unsigned>(mode)};
		harness.check({
			.name = name,
			.instruction =
				mte::store_pair_tag(mode, 1, 1, 2, 3),
			.initial = initial,
			.expected = initial,
			.event = test::ExpectedEvent::memory_fault,
			.native = mte::native_mte,
			.fault = {
				.address = effective,
				.access = mte::arm64::MemoryAccess::write,
			},
			.compare_fault = true,
		});
	}

	auto unmapped = mte::initial_state();
	unmapped.registers[1] = mte::data_base + 0x300;
	harness.check({
		.name = "stgp-unmapped-data",
		.instruction = mte::store_pair_tag(
			mte::AddressMode::post_index, 1, 1, 2, 3),
		.initial = unmapped,
		.expected = unmapped,
		.event = test::ExpectedEvent::memory_fault,
		.native = mte::native_mte,
		.fault = {
			.address = mte::data_base + 0x300,
			.access = mte::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
	});

	auto sp = mte::initial_state();
	sp.sp = mte::data_base + 1;
	harness.check({
		.name = "stgp-sp-alignment-before-address",
		.instruction = mte::store_pair_tag(
			mte::AddressMode::pre_index, 1, 31, 2, 3),
		.initial = sp,
		.expected = sp,
		.event = test::ExpectedEvent::memory_fault,
		.native = mte::native_mte,
		.fault = {
			.address = mte::data_base + 1,
			.access = mte::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
	});

	/* opc==01, VR==0 in the no-allocate pair class is UNALLOCATED. */
	harness.check_illegal("stgp-neighbor-no-allocate-reserved",
		0x6800'0000U, mte::initial_state(), mte::native_mte);
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
	test::EngineHarness harness;
	all_register_fields(harness);
	all_immediates(harness);
	alignment_mapping_and_reserved_faults(harness);
	production_specialization();
	harness.print_summary("arm64-base-mte-stgp");
}
