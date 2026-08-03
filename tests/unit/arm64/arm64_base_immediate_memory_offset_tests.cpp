// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_immediate_memory_test_support.hpp"

#include <algorithm>
#include <cstdio>

namespace test = uml::emulator::tests::base_immediate_memory;
namespace engine_tests = uml::emulator::tests;

namespace {

void unsigned_loads(
	engine_tests::EngineHarness &harness, const test::Variant &variant,
	unsigned variant_index)
{
	constexpr unsigned batch_size = 29;
	const auto base_address = test::data_base + 64U;
	std::vector<std::byte> memory(test::unsigned_region_size());
	test::fill_memory(memory, variant_index);

	for (unsigned first = 0; first < 4096; first += batch_size) {
		const auto count = std::min(batch_size, 4096U - first);
		auto initial = test::seeded_state(
			0x1000'0000'0000'0000ULL |
			(static_cast<std::uint64_t>(variant_index) << 32) | first);
		initial.registers[30] = base_address;
		auto expected = initial;
		std::vector<std::uint32_t> instructions;
		instructions.reserve(count);
		for (unsigned index = 0; index < count; ++index) {
			const auto imm12 = first + index;
			const auto address = test::address_for(
				variant, test::Form::unsigned_offset,
				base_address, imm12);
			const auto raw = test::decode_little_endian(
				memory, address - test::data_base, variant.size);
			expected.registers[index] =
				test::expected_load(variant, raw);
			instructions.push_back(test::encode(
				variant, test::Form::unsigned_offset,
				imm12, 30, index));
		}
		char name[96];
		std::snprintf(name, sizeof(name),
			"%.*s unsigned offsets %u..%u",
			static_cast<int>(variant.name.size()), variant.name.data(),
			first, first + count - 1U);
		test::check_program(harness, name, instructions, initial, expected,
			test::data_base, memory, memory);
	}
}

void unsigned_stores(
	engine_tests::EngineHarness &harness, const test::Variant &variant,
	unsigned variant_index)
{
	constexpr unsigned batch_size = 60;
	const auto base_address = test::data_base + 64U;
	std::vector<std::byte> memory(test::unsigned_region_size());
	test::fill_memory(memory, variant_index);

	for (unsigned first = 0; first < 4096; first += batch_size) {
		const auto count = std::min(batch_size, 4096U - first);
		auto initial = test::seeded_state(
			0x2000'0000'0000'0000ULL |
			(static_cast<std::uint64_t>(variant_index) << 32) | first);
		initial.registers[29] = test::store_value;
		initial.registers[30] = base_address;
		auto expected = initial;
		auto expected_memory = memory;
		std::vector<std::uint32_t> instructions;
		instructions.reserve(count);
		for (unsigned index = 0; index < count; ++index) {
			const auto imm12 = first + index;
			const auto address = test::address_for(
				variant, test::Form::unsigned_offset,
				base_address, imm12);
			test::encode_little_endian(expected_memory,
				address - test::data_base, variant.size,
				test::store_value);
			instructions.push_back(test::encode(
				variant, test::Form::unsigned_offset,
				imm12, 30, 29));
		}
		char name[96];
		std::snprintf(name, sizeof(name),
			"%.*s unsigned offsets %u..%u",
			static_cast<int>(variant.name.size()), variant.name.data(),
			first, first + count - 1U);
		test::check_program(harness, name, instructions, initial, expected,
			test::data_base, memory, expected_memory);
	}
}

void non_writeback_loads(
	engine_tests::EngineHarness &harness, const test::Variant &variant,
	unsigned variant_index, test::Form form)
{
	constexpr unsigned batch_size = 29;
	const auto base_address = test::data_base + test::page_size / 2U;
	std::vector<std::byte> memory(test::page_size);
	test::fill_memory(memory, variant_index +
		static_cast<unsigned>(form) * 17U);

	for (unsigned first = 0; first < 512; first += batch_size) {
		const auto count = std::min(batch_size, 512U - first);
		auto initial = test::seeded_state(
			0x3000'0000'0000'0000ULL |
			(static_cast<std::uint64_t>(variant_index) << 32) |
			(static_cast<std::uint64_t>(
				static_cast<unsigned>(form)) << 24) | first);
		initial.registers[30] = base_address;
		auto expected = initial;
		std::vector<std::uint32_t> instructions;
		instructions.reserve(count);
		for (unsigned index = 0; index < count; ++index) {
			const auto imm9 = first + index;
			const auto address = test::address_for(
				variant, form, base_address, imm9);
			const auto raw = test::decode_little_endian(
				memory, address - test::data_base, variant.size);
			expected.registers[index] =
				test::expected_load(variant, raw);
			instructions.push_back(test::encode(
				variant, form, imm9, 30, index));
		}
		char name[104];
		std::snprintf(name, sizeof(name),
			"%.*s %.*s offsets %u..%u",
			static_cast<int>(variant.name.size()), variant.name.data(),
			static_cast<int>(test::form_name(form).size()),
			test::form_name(form).data(), first, first + count - 1U);
		test::check_program(harness, name, instructions, initial, expected,
			test::data_base, memory, memory);
	}
}

void non_writeback_stores(
	engine_tests::EngineHarness &harness, const test::Variant &variant,
	unsigned variant_index, test::Form form)
{
	constexpr unsigned batch_size = 60;
	const auto base_address = test::data_base + test::page_size / 2U;
	std::vector<std::byte> memory(test::page_size);
	test::fill_memory(memory, variant_index +
		static_cast<unsigned>(form) * 17U);

	/* Equal residues are at least one transfer width apart, so no store in a
	 * grouped program can hide another offset's expected result.
	 */
	for (unsigned residue = 0; residue < variant.size; ++residue) {
		std::vector<unsigned> offsets;
		for (unsigned imm9 = residue; imm9 < 512; imm9 += variant.size)
			offsets.push_back(imm9);
		for (unsigned first = 0; first < offsets.size();
		     first += batch_size) {
			const auto count = std::min<std::size_t>(
				batch_size, offsets.size() - first);
			auto initial = test::seeded_state(
				0x4000'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(
					variant_index) << 32) |
				(static_cast<std::uint64_t>(
					static_cast<unsigned>(form)) << 24) |
				(static_cast<std::uint64_t>(
					residue) << 16) | first);
			initial.registers[29] = test::store_value;
			initial.registers[30] = base_address;
			auto expected = initial;
			auto expected_memory = memory;
			std::vector<std::uint32_t> instructions;
			instructions.reserve(count);
			for (unsigned index = 0; index < count; ++index) {
				const auto imm9 = offsets[first + index];
				const auto address = test::address_for(
					variant, form, base_address, imm9);
				test::encode_little_endian(expected_memory,
					address - test::data_base,
					variant.size, test::store_value);
				instructions.push_back(test::encode(
					variant, form, imm9, 30, 29));
			}
			char name[112];
			std::snprintf(name, sizeof(name),
				"%.*s %.*s residue=%u group=%u",
				static_cast<int>(variant.name.size()),
				variant.name.data(),
				static_cast<int>(
					test::form_name(form).size()),
				test::form_name(form).data(), residue,
				first / batch_size);
			test::check_program(harness, name, instructions, initial,
				expected, test::data_base, memory,
				expected_memory);
		}
	}
}

void writeback_loads(
	engine_tests::EngineHarness &harness, const test::Variant &variant,
	unsigned variant_index, test::Form form)
{
	constexpr unsigned batch_size = 15;
	std::vector<std::byte> memory(test::page_size);
	test::fill_memory(memory, variant_index +
		static_cast<unsigned>(form) * 31U);

	for (unsigned first = 0; first < 512; first += batch_size) {
		const auto count = std::min(batch_size, 512U - first);
		auto initial = test::seeded_state(
			0x5000'0000'0000'0000ULL |
			(static_cast<std::uint64_t>(variant_index) << 32) |
			(static_cast<std::uint64_t>(
				static_cast<unsigned>(form)) << 24) | first);
		auto expected = initial;
		std::vector<std::uint32_t> instructions;
		instructions.reserve(count);
		for (unsigned index = 0; index < count; ++index) {
			const auto imm9 = first + index;
			const auto displacement = test::signed_imm9(imm9);
			const auto target =
				test::data_base + 256U + index * 16U;
			const auto base_address =
				form == test::Form::post_index ? target :
				static_cast<std::uint64_t>(
					static_cast<std::int64_t>(target) -
					displacement);
			const unsigned base_register = index;
			const unsigned destination = 15U + index;
			initial.registers[base_register] = base_address;
			expected.registers[base_register] =
				test::writeback_for(form, base_address, imm9);
			const auto raw = test::decode_little_endian(
				memory, target - test::data_base, variant.size);
			expected.registers[destination] =
				test::expected_load(variant, raw);
			instructions.push_back(test::encode(
				variant, form, imm9,
				base_register, destination));
		}
		char name[104];
		std::snprintf(name, sizeof(name),
			"%.*s %.*s offsets %u..%u",
			static_cast<int>(variant.name.size()), variant.name.data(),
			static_cast<int>(test::form_name(form).size()),
			test::form_name(form).data(), first, first + count - 1U);
		test::check_program(harness, name, instructions, initial, expected,
			test::data_base, memory, memory);
	}
}

void writeback_stores(
	engine_tests::EngineHarness &harness, const test::Variant &variant,
	unsigned variant_index, test::Form form)
{
	constexpr unsigned batch_size = 29;
	std::vector<std::byte> memory(test::page_size);
	test::fill_memory(memory, variant_index +
		static_cast<unsigned>(form) * 31U);

	for (unsigned first = 0; first < 512; first += batch_size) {
		const auto count = std::min(batch_size, 512U - first);
		auto initial = test::seeded_state(
			0x6000'0000'0000'0000ULL |
			(static_cast<std::uint64_t>(variant_index) << 32) |
			(static_cast<std::uint64_t>(
				static_cast<unsigned>(form)) << 24) | first);
		initial.registers[30] = test::store_value;
		auto expected = initial;
		auto expected_memory = memory;
		std::vector<std::uint32_t> instructions;
		instructions.reserve(count);
		for (unsigned index = 0; index < count; ++index) {
			const auto imm9 = first + index;
			const auto displacement = test::signed_imm9(imm9);
			const auto target =
				test::data_base + 256U + index * 16U;
			const auto base_address =
				form == test::Form::post_index ? target :
				static_cast<std::uint64_t>(
					static_cast<std::int64_t>(target) -
					displacement);
			initial.registers[index] = base_address;
			expected.registers[index] =
				test::writeback_for(form, base_address, imm9);
			test::encode_little_endian(expected_memory,
				target - test::data_base,
				variant.size, test::store_value);
			instructions.push_back(test::encode(
				variant, form, imm9, index, 30));
		}
		char name[104];
		std::snprintf(name, sizeof(name),
			"%.*s %.*s offsets %u..%u",
			static_cast<int>(variant.name.size()), variant.name.data(),
			static_cast<int>(test::form_name(form).size()),
			test::form_name(form).data(), first, first + count - 1U);
		test::check_program(harness, name, instructions, initial, expected,
			test::data_base, memory, expected_memory);
	}
}

void exhaustive_offsets(engine_tests::EngineHarness &harness)
{
	for (unsigned variant_index = 0;
	     variant_index < test::variants.size(); ++variant_index) {
		const auto &variant = test::variants[variant_index];
		if (variant.load)
			unsigned_loads(harness, variant, variant_index);
		else
			unsigned_stores(harness, variant, variant_index);

		for (const auto form :
		     {test::Form::unscaled, test::Form::unprivileged})
			if (variant.load)
				non_writeback_loads(
					harness, variant, variant_index, form);
			else
				non_writeback_stores(
					harness, variant, variant_index, form);
		for (const auto form :
		     {test::Form::post_index, test::Form::pre_index})
			if (variant.load)
				writeback_loads(
					harness, variant, variant_index, form);
			else
				writeback_stores(
					harness, variant, variant_index, form);
	}
}

} // namespace

int main()
{
	engine_tests::EngineHarness harness;
	exhaustive_offsets(harness);
	test::require_engine_counts(harness, 2746U);
	harness.print_summary("arm64-base-immediate-memory-offset");
	return 0;
}
