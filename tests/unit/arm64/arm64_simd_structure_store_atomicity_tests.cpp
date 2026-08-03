// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_structure_test_support.hpp"

#include <array>
#include <cstdlib>
#include <cstdio>

namespace structure = arm64_simd_structure_test;
namespace engine = uml::emulator::tests;
namespace arm64 = uml::cpu::arm64;

namespace {

constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

[[nodiscard]] constexpr std::uint32_t exclusive(
	bool load, unsigned rn, unsigned rt, unsigned status = 31U)
{
	return 0xc800'0000U | (static_cast<unsigned>(load) << 22) |
		((status & 31U) << 16) | (31U << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] std::uint64_t load_u64(
	std::span<const std::byte> memory, std::size_t offset)
{
	std::uint64_t value = 0;
	for (unsigned byte = 0; byte < 8U; ++byte)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(memory[offset + byte])) <<
			(byte * 8U);
	return value;
}

void validate_failed_exclusive(
	const arm64::CpuState &, const arm64::CpuState &expected,
	const arm64::CpuState &actual, std::string_view case_name,
	std::string_view engine_name, std::uint32_t instruction)
{
	auto normalized = actual;
	normalized.exclusive_value = expected.exclusive_value;
	normalized.exclusive_value2 = expected.exclusive_value2;
	normalized.exclusive_version = expected.exclusive_version;
	engine::require_architecture_equal(expected, normalized, case_name,
		engine_name, instruction);
}

void check_store(engine::EngineHarness &harness, std::string_view name,
	std::uint32_t store_instruction, const arm64::CpuState &initial,
	const arm64::CpuState &expected,
	std::span<const std::byte> initial_memory,
	std::span<const std::byte> expected_memory,
	std::span<const engine::AllocationTag> initial_tags = {},
	std::span<const engine::AllocationTag> expected_tags = {})
{
	const std::array code{
		engine::ProgramWord{0, exclusive(true, 2, 5)},
		engine::ProgramWord{4, store_instruction},
		engine::ProgramWord{8, exclusive(false, 2, 7, 6)},
		engine::ProgramWord{12, terminal_system_call},
	};
	const std::array regions{
		engine::MemoryRegion{
			.address = structure::data_base,
			.initial = initial_memory,
			.expected = expected_memory,
			.initial_tags = initial_tags,
			.expected_tags = expected_tags,
		},
	};
	harness.check_program({
		.name = name,
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = structure::native_requirement(),
		.memory = std::span<const engine::MemoryRegion>{regions},
		.validator = &validate_failed_exclusive,
	});
}

void expect_failed_exclusive(arm64::CpuState &expected,
	std::span<const std::byte> memory, std::uint64_t reservation_address)
{
	expected.registers[5] = load_u64(
		memory, static_cast<std::size_t>(
			reservation_address - structure::data_base));
	expected.registers[6] = 1;
	expected.exclusive_address = ~std::uint64_t{0};
	expected.exclusive_key = 0;
	expected.exclusive_size = 0;
	expected.pc = engine::engine_test_code_base + 16U;
}

[[nodiscard]] constexpr std::uint64_t tagged(
	std::uint64_t address, std::uint8_t tag)
{
	return (address & 0x00ff'ffff'ffff'ffffULL) |
		(static_cast<std::uint64_t>(tag & 15U) << 56);
}

void multiple_stores_invalidate_reservations(
	engine::EngineHarness &harness, std::uint64_t &seed)
{
	const auto address =
		structure::data_base + structure::page_size / 2U;
	for (const auto &form : structure::multiple_forms)
		for (const bool full_vector : {false, true})
			for (unsigned size = 0; size < 4U; ++size) {
				if (!structure::multiple_arrangement_valid(
					form, full_vector, size))
					continue;
				std::vector<std::byte> memory(structure::page_size);
				structure::fill_memory(memory, seed);
				auto expected_memory = memory;
				auto initial = structure::seeded_state(seed++);
				structure::set_base(initial, 2, address);
				structure::set_base(initial, 3, address);
				auto expected = initial;
				const auto offset =
					static_cast<std::size_t>(
						address - structure::data_base);
				expect_failed_exclusive(expected, memory, address);
				structure::apply_multiple_oracle(
					initial, expected, memory, expected_memory,
					offset, form, full_vector, size, false, 29);
				char name[144];
				std::snprintf(name, sizeof(name),
					"%.*s store invalidates reservation q=%u "
					"size=%u",
					static_cast<int>(form.mnemonic.size()),
					form.mnemonic.data(),
					static_cast<unsigned>(full_vector), size);
				check_store(harness, name,
					structure::multiple_instruction(
						full_vector, false, false, 0,
						form.opcode, size, 3, 29),
					initial, expected, memory, expected_memory);
			}
}

void lane_stores_invalidate_reservations(
	engine::EngineHarness &harness, std::uint64_t &seed)
{
	const auto address =
		structure::data_base + structure::page_size / 2U;
	for (const bool full_vector : {false, true})
		for (unsigned extra = 0; extra < 2U; ++extra)
			for (unsigned opcode = 0; opcode < 6U; ++opcode)
				for (unsigned scale_bit = 0; scale_bit < 2U;
				     ++scale_bit)
					for (unsigned size = 0; size < 4U; ++size) {
						const auto form =
							structure::decode_single(
								full_vector, false,
								extra, opcode,
								scale_bit, size);
						if (!form.valid)
							continue;
						std::vector<std::byte> memory(
							structure::page_size);
						structure::fill_memory(memory, seed);
						auto expected_memory = memory;
						auto initial =
							structure::seeded_state(seed++);
						structure::set_base(
							initial, 2, address);
						structure::set_base(
							initial, 3, address);
						auto expected = initial;
						const auto offset =
							static_cast<std::size_t>(
								address -
								structure::data_base);
						expect_failed_exclusive(
							expected, memory, address);
						structure::apply_single_oracle(
							initial, expected, memory,
							expected_memory, offset,
							form, full_vector, false,
							29);
						char name[160];
						std::snprintf(name, sizeof(name),
							"lane store invalidates reservation "
							"q=%u count=%u width=%zu lane=%zu",
							static_cast<unsigned>(
								full_vector),
							form.register_count,
							form.element_size, form.lane);
						check_store(harness, name,
							structure::
								single_instruction(
									full_vector, false,
									false, extra, 0,
									opcode, scale_bit,
									size, 3, 29),
							initial, expected, memory,
							expected_memory);
					}
}

void cross_page_slow_stores_invalidate_reservations(
	engine::EngineHarness &harness, std::uint64_t &seed)
{
	std::vector<std::byte> memory(2U * structure::page_size);
	structure::fill_memory(memory, seed);
	const auto boundary = structure::data_base + structure::page_size;

	{
		const auto &form = structure::multiple_forms.back();
		const auto transfer =
			structure::multiple_transfer_size(form, true);
		const auto store_address = boundary - transfer / 2U;
		auto expected_memory = memory;
		auto initial = structure::seeded_state(seed++);
		structure::set_base(initial, 2, store_address);
		structure::set_base(initial, 3, store_address);
		auto expected = initial;
		expect_failed_exclusive(expected, memory, store_address);
		structure::apply_multiple_oracle(
			initial, expected, memory, expected_memory,
			static_cast<std::size_t>(
				store_address - structure::data_base),
			form, true, 0, false, 29);
		check_store(
			harness, "multiple cross-page store invalidates reservation",
			structure::multiple_instruction(
				true, false, false, 0, form.opcode, 0, 3, 29),
			initial, expected, memory, expected_memory);
	}

	{
		constexpr unsigned extra = 1;
		constexpr unsigned opcode = 5;
		constexpr unsigned scale_bit = 0;
		constexpr unsigned size = 1;
		const auto form = structure::decode_single(
			true, false, extra, opcode, scale_bit, size);
		if (!form.valid || form.register_count != 4U ||
		    form.element_size != 8U)
			std::abort();
		const auto transfer = form.register_count * form.element_size;
		const auto store_address = boundary - transfer / 2U;
		auto expected_memory = memory;
		auto initial = structure::seeded_state(seed++);
		structure::set_base(initial, 2, store_address);
		structure::set_base(initial, 3, store_address);
		auto expected = initial;
		expect_failed_exclusive(expected, memory, store_address);
		structure::apply_single_oracle(
			initial, expected, memory, expected_memory,
			static_cast<std::size_t>(
				store_address - structure::data_base),
			form, true, false, 29);
		check_store(
			harness, "lane cross-page store invalidates reservation",
			structure::single_instruction(
				true, false, false, extra, 0, opcode, scale_bit,
				size, 3, 29),
			initial, expected, memory, expected_memory);
	}
}

void tagged_slow_stores_invalidate_reservations(
	engine::EngineHarness &harness, std::uint64_t &seed)
{
	constexpr std::uint8_t logical_tag = 9;
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, seed);
	const auto store_address =
		structure::data_base + structure::page_size / 2U;

	{
		const auto &form = structure::multiple_forms.back();
		const auto transfer =
			structure::multiple_transfer_size(form, true);
		std::vector<engine::AllocationTag> tags;
		for (std::size_t offset = 0; offset < transfer; offset += 16U)
			tags.push_back({
				.offset = static_cast<std::size_t>(
					store_address - structure::data_base +
					offset),
				.value = logical_tag,
			});
		auto expected_memory = memory;
		auto initial = structure::seeded_state(seed++);
		initial.set_mte_configuration(true, 0);
		structure::set_base(initial, 2, store_address);
		structure::set_base(
			initial, 3, tagged(store_address, logical_tag));
		auto expected = initial;
		expect_failed_exclusive(expected, memory, store_address);
		structure::apply_multiple_oracle(
			initial, expected, memory, expected_memory,
			static_cast<std::size_t>(
				store_address - structure::data_base),
			form, true, 0, false, 29);
		check_store(
			harness, "multiple tagged store invalidates reservation",
			structure::multiple_instruction(
				true, false, false, 0, form.opcode, 0, 3, 29),
			initial, expected, memory, expected_memory, tags);
	}

	{
		constexpr unsigned extra = 1;
		constexpr unsigned opcode = 5;
		constexpr unsigned scale_bit = 0;
		constexpr unsigned size = 1;
		const auto form = structure::decode_single(
			true, false, extra, opcode, scale_bit, size);
		if (!form.valid || form.register_count != 4U ||
		    form.element_size != 8U)
			std::abort();
		const auto transfer = form.register_count * form.element_size;
		std::vector<engine::AllocationTag> tags;
		for (std::size_t offset = 0; offset < transfer; offset += 16U)
			tags.push_back({
				.offset = static_cast<std::size_t>(
					store_address - structure::data_base +
					offset),
				.value = logical_tag,
			});
		auto expected_memory = memory;
		auto initial = structure::seeded_state(seed++);
		initial.set_mte_configuration(true, 0);
		structure::set_base(initial, 2, store_address);
		structure::set_base(
			initial, 3, tagged(store_address, logical_tag));
		auto expected = initial;
		expect_failed_exclusive(expected, memory, store_address);
		structure::apply_single_oracle(
			initial, expected, memory, expected_memory,
			static_cast<std::size_t>(
				store_address - structure::data_base),
			form, true, false, 29);
		check_store(
			harness, "lane tagged store invalidates reservation",
			structure::single_instruction(
				true, false, false, extra, 0, opcode, scale_bit,
				size, 3, 29),
			initial, expected, memory, expected_memory, tags);
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	std::uint64_t seed = 0xaa00'0000'0000'0000ULL;
	multiple_stores_invalidate_reservations(harness, seed);
	lane_stores_invalidate_reservations(harness, seed);
	cross_page_slow_stores_invalidate_reservations(harness, seed);
	tagged_slow_stores_invalidate_reservations(harness, seed);
	harness.print_summary("arm64-simd-structure-store-atomicity");
	return 0;
}
