// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_immediate_memory_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = uml::emulator::tests::base_immediate_memory;
namespace engine_tests = uml::emulator::tests;

namespace {

[[nodiscard]] unsigned representative_immediate(test::Form form)
{
	return form == test::Form::unsigned_offset ? 3U :
		test::encode_imm9(-7);
}

void signed_boundaries(engine_tests::EngineHarness &harness)
{
	for (unsigned variant_index = 0;
	     variant_index < test::variants.size(); ++variant_index) {
		const auto &variant = test::variants[variant_index];
		if (!variant.load || variant.operation < 2U)
			continue;
		const auto bits = static_cast<unsigned>(variant.size * 8U);
		const auto sign = std::uint64_t{1} << (bits - 1U);
		const std::array values{
			std::uint64_t{0},
			sign - 1U,
			sign,
			test::low_mask(bits),
		};
		for (const auto form : test::all_forms)
			for (unsigned value_index = 0;
			     value_index < values.size(); ++value_index) {
				std::vector<std::byte> memory(test::page_size);
				test::fill_memory(memory,
					variant_index * 19U + value_index);
				const auto target =
					test::data_base + test::page_size / 2U + 1U;
				test::encode_little_endian(memory,
					target - test::data_base,
					variant.size, values[value_index]);
				const auto immediate =
					representative_immediate(form);
				auto initial = test::seeded_state(
					0x9000'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(
						variant_index) << 16) |
					(static_cast<std::uint64_t>(
						static_cast<unsigned>(
							form)) << 8) |
					value_index);
				initial.registers[4] = test::base_for_address(
					variant, form, target, immediate);
				initial.registers[5] =
					0xffff'ffff'ffff'ffffULL;
				auto expected = initial;
				auto expected_memory = memory;
				test::apply_oracle(variant, form, immediate, 4, 5,
					expected, expected_memory,
					test::data_base);
				char name[112];
				std::snprintf(name, sizeof(name),
					"%.*s %.*s signed boundary %u",
					static_cast<int>(variant.name.size()),
					variant.name.data(),
					static_cast<int>(
						test::form_name(form).size()),
					test::form_name(form).data(),
					value_index);
				test::check_memory(harness, name,
					test::encode(variant, form,
						immediate, 4, 5),
					initial, expected, test::data_base,
					memory, expected_memory);
			}
	}
}

void stack_and_zero_registers(engine_tests::EngineHarness &harness)
{
	for (unsigned variant_index = 0;
	     variant_index < test::variants.size(); ++variant_index) {
		const auto &variant = test::variants[variant_index];
		for (const auto form : test::all_forms) {
			std::vector<std::byte> memory(test::page_size);
			test::fill_memory(memory, variant_index * 7U +
				static_cast<unsigned>(form));
			auto expected_memory = memory;
			const auto immediate =
				form == test::Form::unsigned_offset ? 2U :
				test::encode_imm9(16);
			auto initial = test::seeded_state(
				0x9100'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(
					variant_index) << 8) |
				static_cast<unsigned>(form));
			initial.sp = test::data_base + test::page_size / 2U;
			test::require((initial.sp & 15U) == 0U,
				"aligned SP test setup is not aligned");
			auto expected = initial;
			test::apply_oracle(variant, form, immediate, 31, 31,
				expected, expected_memory, test::data_base);
			char name[104];
			std::snprintf(name, sizeof(name),
				"%.*s %.*s SP/XZR",
				static_cast<int>(variant.name.size()),
				variant.name.data(),
				static_cast<int>(test::form_name(form).size()),
				test::form_name(form).data());
			test::check_memory(harness, name,
				test::encode(variant, form, immediate, 31, 31),
				initial, expected, test::data_base,
				memory, expected_memory);
		}
	}
}

void transfer_register_sweep(engine_tests::EngineHarness &harness)
{
	for (unsigned variant_index = 0;
	     variant_index < test::variants.size(); ++variant_index) {
		const auto &variant = test::variants[variant_index];
		for (const auto form : test::all_forms)
			for (unsigned rt = 0; rt < 32; ++rt) {
				std::vector<std::byte> memory(test::page_size);
				test::fill_memory(memory, variant_index * 37U + rt);
				auto expected_memory = memory;
				const auto target = test::data_base +
					test::page_size / 2U + 3U;
				const auto immediate =
					representative_immediate(form);
				auto initial = test::seeded_state(
					0x9200'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(
						variant_index) << 16) |
					(static_cast<std::uint64_t>(
						static_cast<unsigned>(
							form)) << 8) | rt);
				initial.registers[29] = test::base_for_address(
					variant, form, target, immediate);
				auto expected = initial;
				test::apply_oracle(variant, form, immediate, 29, rt,
					expected, expected_memory,
					test::data_base);
				char name[112];
				std::snprintf(name, sizeof(name),
					"%.*s %.*s Rt=%u",
					static_cast<int>(variant.name.size()),
					variant.name.data(),
					static_cast<int>(
						test::form_name(form).size()),
					test::form_name(form).data(), rt);
				test::check_memory(harness, name,
					test::encode(variant, form,
						immediate, 29, rt),
					initial, expected, test::data_base,
					memory, expected_memory);
			}
	}
}

void base_register_sweep(engine_tests::EngineHarness &harness)
{
	for (unsigned variant_index = 0;
	     variant_index < test::variants.size(); ++variant_index) {
		const auto &variant = test::variants[variant_index];
		for (const auto form : test::all_forms)
			for (unsigned rn = 0; rn < 31; ++rn) {
				std::vector<std::byte> memory(test::page_size);
				test::fill_memory(memory, variant_index * 43U + rn);
				auto expected_memory = memory;
				const auto target = test::data_base +
					test::page_size / 2U + 5U;
				const auto immediate =
					representative_immediate(form);
				auto initial = test::seeded_state(
					0x9300'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(
						variant_index) << 16) |
					(static_cast<std::uint64_t>(
						static_cast<unsigned>(
							form)) << 8) | rn);
				const auto base = test::base_for_address(
					variant, form, target, immediate);
				initial.write_register(rn, base, true);
				auto expected = initial;
				test::apply_oracle(variant, form, immediate, rn, 28,
					expected, expected_memory,
					test::data_base);
				char name[112];
				std::snprintf(name, sizeof(name),
					"%.*s %.*s Rn=%u",
					static_cast<int>(variant.name.size()),
					variant.name.data(),
					static_cast<int>(
						test::form_name(form).size()),
					test::form_name(form).data(), rn);
				test::check_memory(harness, name,
					test::encode(variant, form,
						immediate, rn, 28),
					initial, expected, test::data_base,
					memory, expected_memory);
			}
	}
}

void writeback_overlap(engine_tests::EngineHarness &harness)
{
	for (unsigned variant_index = 0;
	     variant_index < test::variants.size(); ++variant_index) {
		const auto &variant = test::variants[variant_index];
		for (const auto form :
		     {test::Form::post_index, test::Form::pre_index})
			for (unsigned reg = 0; reg < 31; ++reg) {
				std::vector<std::byte> memory(test::page_size);
				test::fill_memory(memory,
					variant_index * 47U + reg);
				auto expected_memory = memory;
				const auto target = test::data_base +
					test::page_size / 2U + 7U;
				const auto immediate =
					test::encode_imm9(17);
				auto initial = test::seeded_state(
					0x9400'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(
						variant_index) << 16) |
					(static_cast<std::uint64_t>(
						static_cast<unsigned>(
							form)) << 8) | reg);
				initial.registers[reg] = test::base_for_address(
					variant, form, target, immediate);
				auto expected = initial;
				test::apply_oracle(variant, form, immediate,
					reg, reg, expected, expected_memory,
					test::data_base);
				char name[112];
				std::snprintf(name, sizeof(name),
					"%.*s %.*s Rn=Rt=%u",
					static_cast<int>(variant.name.size()),
					variant.name.data(),
					static_cast<int>(
						test::form_name(form).size()),
					test::form_name(form).data(), reg);
				test::check_memory(harness, name,
					test::encode(variant, form,
						immediate, reg, reg),
					initial, expected, test::data_base,
					memory, expected_memory);
			}
	}
}

void cross_page_success(engine_tests::EngineHarness &harness)
{
	for (unsigned variant_index = 0;
	     variant_index < test::variants.size(); ++variant_index) {
		const auto &variant = test::variants[variant_index];
		if (variant.size == 1U)
			continue;
		for (const auto form : test::all_forms) {
			std::vector<std::byte> memory(2U * test::page_size);
			test::fill_memory(memory, variant_index * 53U +
				static_cast<unsigned>(form));
			auto expected_memory = memory;
			const auto target = test::data_base +
				test::page_size - variant.size / 2U;
			const unsigned immediate = 0;
			auto initial = test::seeded_state(
				0x9500'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(
					variant_index) << 8) |
				static_cast<unsigned>(form));
			initial.registers[2] = target;
			initial.registers[3] = test::store_value;
			auto expected = initial;
			test::apply_oracle(variant, form, immediate, 2, 3,
				expected, expected_memory, test::data_base);
			char name[104];
			std::snprintf(name, sizeof(name),
				"%.*s %.*s cross-page success",
				static_cast<int>(variant.name.size()),
				variant.name.data(),
				static_cast<int>(test::form_name(form).size()),
				test::form_name(form).data());
			test::check_memory(harness, name,
				test::encode(variant, form, immediate, 2, 3),
				initial, expected, test::data_base,
				memory, expected_memory);
		}
	}
}

void top_byte_ignore_without_mte(engine_tests::EngineHarness &harness)
{
	constexpr std::uint64_t top_byte = std::uint64_t{2} << 56U;
	for (const unsigned variant_index : {0U, 1U}) {
		const auto &variant = test::variants[variant_index];
		for (const auto form : test::all_forms) {
			std::vector<std::byte> memory(test::page_size);
			test::fill_memory(memory, 61U + variant_index * 7U +
				static_cast<unsigned>(form));
			auto expected_memory = memory;
			const auto target = test::data_base + test::page_size / 2U;
			const unsigned immediate =
				form == test::Form::unsigned_offset ? 3U :
				test::encode_imm9(7);
			const auto physical_base = test::base_for_address(
				variant, form, target, immediate);
			const auto tagged_base = physical_base | top_byte;
			auto initial = test::seeded_state(
				0x9600'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(variant_index) << 8U) |
				static_cast<unsigned>(form));
			initial.tbi_enabled = true;
			initial.registers[4] = tagged_base;
			initial.registers[5] = test::store_value;
			auto expected = initial;
			const auto offset = static_cast<std::size_t>(
				target - test::data_base);
			if (variant.load) {
				expected.write_register(5,
					test::expected_load(variant,
						test::decode_little_endian(
							expected_memory, offset,
							variant.size)),
					false);
			} else {
				test::encode_little_endian(expected_memory, offset,
					variant.size, test::store_value);
			}
			if (test::writes_back(form))
				expected.registers[4] = test::writeback_for(
					form, tagged_base, immediate);
			char name[112];
			std::snprintf(name, sizeof(name),
				"%.*s %.*s TBI without MTE",
				static_cast<int>(variant.name.size()),
				variant.name.data(),
				static_cast<int>(test::form_name(form).size()),
				test::form_name(form).data());
			test::check_memory(harness, name,
				test::encode(variant, form, immediate, 4, 5),
				initial, expected, test::data_base,
				memory, expected_memory);
		}
	}
}

} // namespace

int main()
{
	engine_tests::EngineHarness harness;
	signed_boundaries(harness);
	stack_and_zero_registers(harness);
	transfer_register_sweep(harness);
	base_register_sweep(harness);
	writeback_overlap(harness);
	cross_page_success(harness);
	top_byte_ignore_without_mte(harness);
	test::require_engine_counts(harness, 5121U);
	harness.print_summary("arm64-base-immediate-memory-behavior");
	return 0;
}
