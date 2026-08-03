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

[[nodiscard]] constexpr bool two_tags(
	mte::TagMemoryOperation operation) noexcept
{
	return operation == mte::TagMemoryOperation::store_two_tags;
}

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

void check_valid(test::EngineHarness &harness,
	mte::TagMemoryOperation operation, mte::AddressMode mode,
	unsigned imm9, unsigned base, unsigned transfer, std::string_view name)
{
	mte::Page initial_memory;
	mte::fill_pattern(initial_memory,
		static_cast<unsigned>(operation) * 31U +
		static_cast<unsigned>(mode) * 7U + imm9);
	const auto expected_memory = initial_memory;
	const auto target_offset = (mte::page_size / 2) & ~std::size_t{31};
	const auto target = mte::data_base + target_offset;
	auto initial = mte::initial_state();
	const auto base_tag = (base * 5U + transfer + 1U) & 15U;
	const auto base_address =
		(mode == mte::AddressMode::post_index ?
		 target : target - mte::offset9(imm9)) |
		(static_cast<std::uint64_t>(base_tag) << 56);
	mte::write_register(initial, base, base_address, true);
	if (transfer != base) {
		const auto source =
			(static_cast<std::uint64_t>(
				(transfer * 11U + base * 3U + 7U) & 15U) << 56) |
			0x0012'3456'789a'bcd0ULL;
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
		two_tags(operation) ?
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
					const unsigned imm9 = (base * 73U +
						transfer * 19U +
						static_cast<unsigned>(mode) * 7U) &
						0x1ffU;
					const mte::CaseName name{
						"%s-%u-b%u-t%u-i%u",
						two_tags(operation) ? "st2g" : "stg",
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
					two_tags(operation) ? "st2g" : "stg",
					static_cast<unsigned>(mode), imm9};
				check_valid(
					harness, operation, mode, imm9, 1, 2, name);
			}
		}
	}
}

void alignment_and_first_access_faults(test::EngineHarness &harness)
{
	for (const auto operation : operations) {
		for (const auto mode : modes) {
			auto initial = mte::initial_state();
			const auto effective = mte::data_base + 0x301;
			initial.registers[1] =
				mode == mte::AddressMode::post_index ?
					effective : effective - 16;
			initial.registers[2] = 0x0d00'0000'0000'0000ULL;
			const auto instruction =
				mte::tag_memory(operation, mode, 1, 1, 2);
			const mte::CaseName name{"%s-%u-unaligned",
				two_tags(operation) ? "st2g" : "stg",
				static_cast<unsigned>(mode)};
			harness.check({
				.name = name,
				.instruction = instruction,
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

	for (const auto operation : operations) {
		auto initial = mte::initial_state();
		initial.registers[1] = mte::data_base + 0x300;
		initial.registers[2] = 0x0e00'0000'0000'0000ULL;
		const mte::CaseName name{"%s-unmapped",
			two_tags(operation) ? "st2g" : "stg"};
		harness.check({
			.name = name,
			.instruction = mte::tag_memory(operation,
				mte::AddressMode::signed_offset, 0, 1, 2),
			.initial = initial,
			.expected = initial,
			.event = test::ExpectedEvent::memory_fault,
			.native = mte::native_mte,
			.fault = {
				.address = mte::data_base + 0x300,
				.access = mte::arm64::MemoryAccess::write,
			},
			.compare_fault = true,
		});
	}

	auto initial = mte::initial_state();
	initial.sp = mte::data_base + 1;
	initial.registers[2] = 0x0f00'0000'0000'0000ULL;
	harness.check({
		.name = "stg-sp-alignment-before-address",
		.instruction = mte::tag_memory(mte::TagMemoryOperation::store_tag,
			mte::AddressMode::pre_index, 1, 31, 2),
		.initial = initial,
		.expected = initial,
		.event = test::ExpectedEvent::memory_fault,
		.native = mte::native_mte,
		.fault = {
			.address = mte::data_base + 1,
			.access = mte::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
	});
}

void second_tag_fault_is_restart_safe(test::EngineHarness &harness)
{
	mte::Page initial_memory;
	mte::fill_pattern(initial_memory, 71);
	const auto expected_memory = initial_memory;
	auto initial = mte::initial_state();
	const auto target = mte::data_base + mte::page_size - 16;
	initial.registers[1] = target;
	initial.registers[2] = 0x0e00'0000'0000'0000ULL;
	const std::array initial_tags{
		test::AllocationTag{.offset = mte::page_size - 16, .value = 3},
	};
	const std::array expected_tags{
		test::AllocationTag{.offset = mte::page_size - 16, .value = 14},
	};
	const std::array regions{mte::memory_region(
		initial_memory, expected_memory, initial_tags, expected_tags)};
	harness.check({
		.name = "st2g-second-tag-fault",
		.instruction = mte::tag_memory(
			mte::TagMemoryOperation::store_two_tags,
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
	test::EngineHarness harness;
	all_register_fields(harness);
	all_immediates(harness);
	alignment_and_first_access_faults(harness);
	second_tag_fault_is_restart_safe(harness);
	production_specialization();
	harness.print_summary("arm64-base-mte-tag-store");
}
