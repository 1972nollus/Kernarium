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

[[nodiscard]] constexpr bool two_granules(
	mte::TagMemoryOperation operation) noexcept
{
	return operation ==
		mte::TagMemoryOperation::store_zero_two_tags;
}

[[nodiscard]] constexpr std::size_t transfer_size(
	mte::TagMemoryOperation operation) noexcept
{
	return two_granules(operation) ? 32 : 16;
}

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

void check_valid(test::EngineHarness &harness,
	mte::TagMemoryOperation operation, mte::AddressMode mode,
	unsigned imm9, unsigned base, unsigned transfer, std::string_view name)
{
	mte::Page initial_memory;
	mte::fill_pattern(initial_memory,
		static_cast<unsigned>(operation) * 41U +
		static_cast<unsigned>(mode) * 17U + imm9);
	auto expected_memory = initial_memory;
	const auto target_offset = (mte::page_size / 2) & ~std::size_t{31};
	const auto target = mte::data_base + target_offset;
	auto initial = mte::initial_state();
	const auto base_address =
		(mode == mte::AddressMode::post_index ?
		 target : target - mte::offset9(imm9)) |
		(static_cast<std::uint64_t>(
			(base * 3U + transfer + 2U) & 15U) << 56);
	mte::write_register(initial, base, base_address, true);
	if (transfer != base) {
		const auto source =
			(static_cast<std::uint64_t>(
				(transfer * 9U + base + 5U) & 15U) << 56) |
			0x0000'1234'5678'9ab0ULL;
		mte::write_register(initial, transfer, source, true);
	}
	const auto tag = static_cast<std::uint8_t>(
		mte::logical_tag(mte::read_register(initial, transfer, true)));
	auto expected = initial;
	if (mode == mte::AddressMode::post_index ||
	    mode == mte::AddressMode::pre_index) {
		mte::write_register(expected, base,
			base_address + mte::offset9(imm9), true);
	}
	std::fill_n(expected_memory.begin() + target_offset,
		transfer_size(operation), std::byte{});
	const std::array initial_tags{
		test::AllocationTag{.offset = target_offset, .value = 1},
		test::AllocationTag{.offset = target_offset + 16, .value = 2},
	};
	const std::array expected_one{
		test::AllocationTag{.offset = target_offset, .value = tag},
	};
	const std::array expected_two{
		test::AllocationTag{.offset = target_offset, .value = tag},
		test::AllocationTag{.offset = target_offset + 16, .value = tag},
	};
	const auto expected_tags =
		two_granules(operation) ?
			std::span<const test::AllocationTag>{expected_two} :
			std::span<const test::AllocationTag>{expected_one};
	const std::array regions{mte::memory_region(
		initial_memory, expected_memory, initial_tags, expected_tags)};
	harness.check({
		.name = name,
		.instruction =
			mte::tag_memory(operation, mode, imm9, base, transfer),
		.initial = initial,
		.expected = expected,
		.native = mte::native_mte,
		.memory = regions,
	});
}

void all_register_fields(test::EngineHarness &harness)
{
	for (const auto operation : operations) {
		for (const auto mode : modes) {
			for (unsigned base = 0; base < 32; ++base) {
				for (unsigned transfer = 0; transfer < 32; ++transfer) {
					const unsigned imm9 =
						(base * 29U + transfer * 13U) &
						0x1ffU;
					const mte::CaseName name{
						"%s-%u-b%u-t%u-i%u",
						two_granules(operation) ?
							"stz2g" : "stzg",
						static_cast<unsigned>(mode), base,
						transfer, imm9};
					check_valid(harness, operation, mode,
						imm9, base, transfer, name);
				}
			}
		}
	}
}

void all_immediates(test::EngineHarness &harness)
{
	for (const auto operation : operations) {
		for (const auto mode : modes) {
			for (unsigned imm9 = 0; imm9 < 512; ++imm9) {
				const mte::CaseName name{"%s-%u-imm%u",
					two_granules(operation) ? "stz2g" : "stzg",
					static_cast<unsigned>(mode), imm9};
				check_valid(
					harness, operation, mode, imm9, 1, 2, name);
			}
		}
	}
}

void alignment_and_mapping_faults(test::EngineHarness &harness)
{
	for (const auto operation : operations) {
		for (const auto mode : modes) {
			auto initial = mte::initial_state();
			const auto effective = mte::data_base + 0x301;
			initial.registers[1] =
				mode == mte::AddressMode::post_index ?
					effective : effective - 16;
			initial.registers[2] = 0x0d00'0000'0000'0000ULL;
			const mte::CaseName name{"%s-%u-unaligned",
				two_granules(operation) ? "stz2g" : "stzg",
				static_cast<unsigned>(mode)};
			harness.check({
				.name = name,
				.instruction = mte::tag_memory(
					operation, mode, 1, 1, 2),
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
	}

	mte::Page initial_memory;
	mte::fill_pattern(initial_memory, 83);
	const auto expected_memory = initial_memory;
	auto initial = mte::initial_state();
	const auto boundary = mte::data_base + mte::page_size - 16;
	initial.registers[1] = boundary;
	initial.registers[2] = 0x0c00'0000'0000'0000ULL;
	const std::array initial_tags{
		test::AllocationTag{.offset = mte::page_size - 16, .value = 5},
	};
	const std::array regions{mte::memory_region(
		initial_memory, expected_memory, initial_tags)};
	harness.check({
		.name = "stz2g-cross-page-data-fault",
		.instruction = mte::tag_memory(
			mte::TagMemoryOperation::store_zero_two_tags,
			mte::AddressMode::post_index, 1, 1, 2),
		.initial = initial,
		.expected = initial,
		.event = test::ExpectedEvent::memory_fault,
		.native = mte::native_mte,
		.fault = {
			.address = mte::data_base + mte::page_size,
			.access = mte::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
		.memory = regions,
	});

	auto sp = mte::initial_state();
	sp.sp = mte::data_base + 1;
	sp.registers[2] = 0x0f00'0000'0000'0000ULL;
	harness.check({
		.name = "stzg-sp-alignment-before-data",
		.instruction = mte::tag_memory(
			mte::TagMemoryOperation::store_zero_tag,
			mte::AddressMode::signed_offset, 0, 31, 2),
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
	test::EngineHarness harness;
	all_register_fields(harness);
	all_immediates(harness);
	alignment_and_mapping_faults(harness);
	production_specialization();
	harness.print_summary("arm64-base-mte-zero-store");
}
