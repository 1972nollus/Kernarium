// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_mte_test_support.hpp"

#include <array>
#include <cstdint>
#include <set>

namespace mte = arm64_base_mte_test;
namespace test = uml::backend::tests;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

void encoding_examples()
{
	mte::require(mte::load_tag(0, 1, 0) == 0xd960'0020U);
	mte::require(mte::load_tag(0x100, 31, 2) == 0xd970'03e2U);
	mte::require(mte::load_tag(0x1ff, 30, 31) == 0xd97f'f3dfU);
}

void check_valid(test::EngineHarness &harness, unsigned imm9,
	unsigned base, unsigned destination, unsigned tag, unsigned low_bits,
	std::string_view name)
{
	mte::Page initial_memory;
	mte::fill_pattern(initial_memory, imm9 + base + destination);
	const auto expected_memory = initial_memory;
	const auto target_offset = (mte::page_size / 2) & ~std::size_t{15};
	const auto target = mte::data_base + target_offset;
	auto initial = mte::initial_state();
	const auto effective = target + (low_bits & 15U);
	const auto base_address = effective - mte::offset9(imm9);
	mte::write_register(initial, base, base_address, true);
	auto expected = initial;
	if (destination != 31) {
		expected.registers[destination] = mte::with_allocation_tag(
			initial.registers[destination], tag);
	}
	const std::array initial_tags{
		test::AllocationTag{.offset = target_offset,
			.value = static_cast<std::uint8_t>(tag & 15U)},
	};
	const std::array regions{mte::memory_region(
		initial_memory, expected_memory, initial_tags)};
	harness.check({
		.name = name,
		.instruction = mte::load_tag(imm9, base, destination),
		.initial = initial,
		.expected = expected,
		.native = mte::native_mte,
		.memory = regions,
	});
}

void all_register_fields(test::EngineHarness &harness)
{
	for (unsigned base = 0; base < 32; ++base) {
		for (unsigned destination = 0; destination < 32; ++destination) {
			const unsigned imm9 =
				(base * 37U + destination * 11U) & 0x1ffU;
			const unsigned tag = (base * 3U + destination * 5U) & 15U;
			const unsigned low_bits = base == 31 ? 0U :
				(base + destination) & 15U;
			const mte::CaseName name{"ldg-b%u-t%u-i%u-l%u",
				base, destination, imm9, low_bits};
			check_valid(harness, imm9, base, destination, tag,
				low_bits, name);
		}
	}
}

void all_immediates_and_granule_alignment(test::EngineHarness &harness)
{
	for (unsigned imm9 = 0; imm9 < 512; ++imm9) {
		const mte::CaseName name{"ldg-imm%u-low%u", imm9, imm9 & 15U};
		check_valid(harness, imm9, 1, 2, imm9 & 15U,
			imm9 & 15U, name);
	}
}

void fault_ordering(test::EngineHarness &harness)
{
	{
		auto initial = mte::initial_state();
		initial.registers[1] = mte::data_base + 0x347;
		const mte::CaseName name{"ldg-unmapped-tag"};
		harness.check({
			.name = name,
			.instruction = mte::load_tag(0, 1, 2),
			.initial = initial,
			.expected = initial,
			.event = test::ExpectedEvent::memory_fault,
			.native = mte::native_mte,
			.fault = {
				.address = (mte::data_base + 0x347) &
					mte::granule_mask,
				.access = mte::arm64::MemoryAccess::read,
			},
			.compare_fault = true,
		});
	}
	{
		auto initial = mte::initial_state();
		initial.sp = mte::data_base + 1;
		harness.check({
			.name = "ldg-sp-alignment-before-address",
			.instruction = mte::load_tag(0x100, 31, 3),
			.initial = initial,
			.expected = initial,
			.event = test::ExpectedEvent::memory_fault,
			.native = mte::native_mte,
			.fault = {
				.address = mte::data_base + 1,
				.access = mte::arm64::MemoryAccess::read,
			},
			.compare_fault = true,
		});
	}
}

void privileged_and_reserved_allocations(test::EngineHarness &harness)
{
	const auto initial = mte::initial_state();
	/* Exact EL0-invalid MTE2 encodings followed by every op2==00
	 * unallocated opc partition with a nonzero imm9.
	 */
	constexpr std::array invalid{
		0xd920'0000U,
		0xd9a0'0000U,
		0xd9e0'0000U,
		0xd920'1000U,
		0xd9a0'1000U,
		0xd9e0'1000U,
	};
	for (unsigned index = 0; index < invalid.size(); ++index) {
		const mte::CaseName name{"tag-memory-reserved-%u", index};
		harness.check_illegal(name, invalid[index], initial, mte::native_mte);
	}
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
	test::EngineHarness harness;
	all_register_fields(harness);
	all_immediates_and_granule_alignment(harness);
	fault_ordering(harness);
	privileged_and_reserved_allocations(harness);
	production_specialization();
	harness.print_summary("arm64-base-mte-ldg");
}
