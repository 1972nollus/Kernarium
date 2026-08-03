// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_lsui_pair_test_support.hpp"

#include <array>
#include <cstdio>
#include <vector>

namespace lsui = arm64_base_lsui_pair_test;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;

namespace {

constexpr std::uint64_t first_value = 0xf123'4567'89ab'cdefULL;
constexpr std::uint64_t second_value = 0x8123'4567'0bad'f00dULL;

void every_variant_and_immediate(
	engine::EngineHarness &harness, lsui::ExpectedCounts &counts)
{
	for (unsigned variant_index = 0; variant_index < lsui::variants.size();
	     ++variant_index) {
		const auto &variant = lsui::variants[variant_index];
		for (unsigned imm7 = 0; imm7 < 128; ++imm7) {
			auto memory = std::vector<std::byte>(lsui::page_size);
			lsui::fill(memory, variant_index * 131U + imm7);
			auto initial = atomic::state(
				100U + variant_index * 128U + imm7);
			initial.registers[20] = lsui::base_address;
			initial.registers[3] = first_value;
			initial.registers[17] = second_value;
			char name[128];
			std::snprintf(name, sizeof(name), "%.*s every imm7=%u",
				static_cast<int>(variant.mnemonic.size()),
				variant.mnemonic.data(), imm7);
			lsui::check(harness, counts, name, variant, imm7, 20, 3, 17,
				initial, lsui::data_page, memory);
		}
	}
}

void every_transfer_register(
	engine::EngineHarness &harness, lsui::ExpectedCounts &counts)
{
	for (unsigned variant_index = 0; variant_index < lsui::variants.size();
	     ++variant_index) {
		const auto &variant = lsui::variants[variant_index];
		for (unsigned first = 0; first < 31; ++first) {
			const unsigned second = (first + 13U) % 31U;
			auto memory = std::vector<std::byte>(lsui::page_size);
			lsui::fill(memory, 0x200U + variant_index * 31U + first);
			auto initial = atomic::state(
				2000U + variant_index * 64U + first);
			initial.registers[first] =
				first_value ^ (static_cast<std::uint64_t>(first) << 40);
			initial.registers[second] =
				second_value ^ (static_cast<std::uint64_t>(second) << 36);
			initial.sp = lsui::base_address;
			const unsigned imm7 = (first * 17U + variant_index) & 0x7fU;
			char name[128];
			std::snprintf(name, sizeof(name),
				"%.*s transfer rt=%u rt2=%u",
				static_cast<int>(variant.mnemonic.size()),
				variant.mnemonic.data(), first, second);
			lsui::check(harness, counts, name, variant, imm7, 31, first,
				second, initial, lsui::data_page, memory);
		}
	}
}

void zero_register_and_stack_mappings(
	engine::EngineHarness &harness, lsui::ExpectedCounts &counts)
{
	for (unsigned variant_index = 0; variant_index < lsui::variants.size();
	     ++variant_index) {
		const auto &variant = lsui::variants[variant_index];
		const unsigned positions = variant.load ? 2U : 3U;
		for (unsigned zero_position = 0; zero_position < positions;
		     ++zero_position) {
			auto memory = std::vector<std::byte>(lsui::page_size);
			lsui::fill(memory,
				0x400U + variant_index * 7U + zero_position);
			auto initial = atomic::state(
				4000U + variant_index * 8U + zero_position);
			initial.sp = lsui::base_address;
			initial.registers[30] = first_value;
			const unsigned first =
				zero_position == 0U || zero_position == 2U ? 31U : 30U;
			const unsigned second =
				zero_position == 1U || zero_position == 2U ? 31U : 29U;
			char name[128];
			std::snprintf(name, sizeof(name),
				"%.*s zero-position=%u",
				static_cast<int>(variant.mnemonic.size()),
				variant.mnemonic.data(), zero_position);
			lsui::check(harness, counts, name, variant, 3, 31, first,
				second, initial, lsui::data_page, memory);
		}
	}
}

void every_base_and_overlap_choice(
	engine::EngineHarness &harness, lsui::ExpectedCounts &counts)
{
	for (unsigned variant_index = 0; variant_index < lsui::variants.size();
	     ++variant_index) {
		const auto &variant = lsui::variants[variant_index];
		for (unsigned rn = 0; rn < 32; ++rn) {
			auto memory = std::vector<std::byte>(lsui::page_size);
			lsui::fill(memory, 0x600U + variant_index * 32U + rn);
			auto initial = atomic::state(
				6000U + variant_index * 64U + rn);
			initial.registers[3] = first_value;
			initial.registers[4] = second_value;
			atomic::write_register(initial, rn, lsui::base_address, true);
			char name[128];
			std::snprintf(name, sizeof(name), "%.*s every base rn=%u",
				static_cast<int>(variant.mnemonic.size()),
				variant.mnemonic.data(), rn);
			lsui::check(harness, counts, name, variant, 2, rn, 3, 4,
				initial, lsui::data_page, memory);
		}
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	lsui::ExpectedCounts counts;
	every_variant_and_immediate(harness, counts);
	every_transfer_register(harness, counts);
	zero_register_and_stack_mappings(harness, counts);
	every_base_and_overlap_choice(harness, counts);
	lsui::require_counts(harness, counts);
	harness.print_summary("arm64-base-lsui-pair-behavior");
}
