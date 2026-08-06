// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_ordered_memory_test_support.hpp"

#include <array>
#include <cstdio>

namespace ordered = arm64_ordered_memory_test;
namespace engine = uml::emulator::tests;

namespace {

void check_success(engine::EngineHarness &harness,
	const ordered::PairVariant &variant, std::uint64_t target, unsigned rn,
	unsigned rt, unsigned rt2, std::uint64_t seed)
{
	if (variant.load && rt == rt2 && rt != 31U)
		return;
	const auto element_size = std::size_t{1} << variant.size_code;
	const auto total_size = 2U * element_size;
	std::vector<std::byte> initial_memory(ordered::page_size);
	ordered::fill_page(initial_memory, static_cast<unsigned>(seed));
	auto expected_memory = initial_memory;

	auto initial = ordered::seeded_state(seed);
	const auto base =
		variant.writeback && !variant.load ?
			target + total_size : target;
	ordered::set_base(initial, rn, base);
	auto expected = initial;
	const auto memory_offset =
		static_cast<std::size_t>(target - ordered::page_base);
	if (variant.load) {
		const auto first = ordered::load_integer(
			initial_memory, memory_offset, element_size);
		const auto second = ordered::load_integer(
			initial_memory, memory_offset + element_size, element_size);
		ordered::write_gpr(expected, rt, first);
		ordered::write_gpr(expected, rt2, second);
	} else {
		const auto first = ordered::read_gpr(initial, rt);
		const auto second = ordered::read_gpr(initial, rt2);
		ordered::store_integer(
			expected_memory, memory_offset, element_size, first);
		ordered::store_integer(expected_memory,
			memory_offset + element_size, element_size, second);
	}
	if (variant.writeback) {
		const bool suppress = variant.load && rn != 31U &&
			(rn == rt || rn == rt2);
		if (!suppress)
			ordered::write_base(expected, rn,
				variant.load ? base + total_size : target);
	}

	const std::array regions{
		engine::MemoryRegion{
			.address = ordered::page_base,
			.initial = std::span<const std::byte>{initial_memory},
			.expected = std::span<const std::byte>{expected_memory},
		},
	};
	char name[144];
	std::snprintf(name, sizeof(name),
		"%.*s behavior address-low=%llu rn=%u rt=%u rt2=%u",
		static_cast<int>(variant.name.size()), variant.name.data(),
		static_cast<unsigned long long>(target & 15U), rn, rt, rt2);
	harness.check({
		.name = name,
		.instruction =
			ordered::pair_instruction(variant, rn, rt, rt2),
		.initial = initial,
		.expected = expected,
		.native = ordered::native(variant),
		.memory = std::span<const engine::MemoryRegion>{regions},
	});
}

void every_register_alias(engine::EngineHarness &harness)
{
	std::uint64_t seed = 0x8100'0000'0000'0000ULL;
	for (const auto &variant : ordered::pair_variants) {
		for (unsigned rn = 0; rn < 32U; ++rn)
			check_success(harness, variant, ordered::address,
				rn, 3, 4, seed++);
		for (unsigned rt = 0; rt < 32U; ++rt)
			check_success(harness, variant, ordered::address,
				2, rt, 4, seed++);
		for (unsigned rt2 = 0; rt2 < 32U; ++rt2)
			check_success(harness, variant, ordered::address,
				2, 3, rt2, seed++);
		check_success(harness, variant, ordered::address,
			31, 31, 31, seed++);
	}
}

void every_element_aligned_position(engine::EngineHarness &harness)
{
	std::uint64_t seed = 0x8200'0000'0000'0000ULL;
	for (const auto &variant : ordered::pair_variants) {
		const auto element_size = std::size_t{1} << variant.size_code;
		for (unsigned position = 0; position < 2U; ++position)
			check_success(harness, variant,
				ordered::address + position * element_size,
				2, 3, 4, seed++);
	}
}

void stores_invalidate_reservations(engine::EngineHarness &harness)
{
	std::uint64_t seed = 0x8300'0000'0000'0000ULL;
	for (const auto &variant : ordered::pair_variants) {
		if (variant.load)
			continue;
		const auto element_size = std::size_t{1} << variant.size_code;
		const auto total_size = 2U * element_size;
		std::vector<std::byte> initial_memory(ordered::page_size);
		ordered::fill_page(initial_memory, static_cast<unsigned>(seed));
		auto expected_memory = initial_memory;
		auto initial = ordered::seeded_state(seed++);
		initial.registers[2] = ordered::address;
		initial.registers[3] =
			variant.writeback ? ordered::address + total_size :
				ordered::address;
		const auto monitored = ordered::load_integer(
			initial_memory, ordered::address - ordered::page_base, 8);
		ordered::store_integer(expected_memory,
			ordered::address - ordered::page_base, element_size,
			initial.registers[4]);
		ordered::store_integer(expected_memory,
			ordered::address - ordered::page_base + element_size,
			element_size, initial.registers[8]);

		auto expected = initial;
		expected.registers[5] = monitored;
		expected.registers[6] = 1;
		if (variant.writeback)
			expected.registers[3] = ordered::address;
		expected.exclusive_address = ~std::uint64_t{0};
		expected.exclusive_key = 0;
		expected.exclusive_size = 0;
		expected.pc = engine::engine_test_code_base + 16;

		const std::array code{
			engine::ProgramWord{0,
				ordered::scalar_exclusive(3, true, 2, 5)},
			engine::ProgramWord{4,
				ordered::pair_instruction(variant, 3, 4, 8)},
			engine::ProgramWord{8,
				ordered::scalar_exclusive(3, false, 2, 7, 6)},
			engine::ProgramWord{12, ordered::terminal_system_call},
		};
		const std::array regions{
			engine::MemoryRegion{
				.address = ordered::page_base,
				.initial = std::span<const std::byte>{initial_memory},
				.expected = std::span<const std::byte>{expected_memory},
			},
		};
		char name[128];
		std::snprintf(name, sizeof(name),
			"%.*s invalidates exclusive reservation",
			static_cast<int>(variant.name.size()), variant.name.data());
		harness.check_program({
			.name = name,
			.code = code,
			.initial = initial,
			.expected = expected,
			.native = ordered::native(variant),
			.memory = std::span<const engine::MemoryRegion>{regions},
			.validator = &ordered::validate_failed_exclusive,
			.validator_relation_id =
				"permitted-exclusive-store-failure/v1",
		});
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	every_register_alias(harness);
	every_element_aligned_position(harness);
	stores_invalidate_reservations(harness);
	harness.print_summary("arm64-base-ordered-pair-behavior");
	return 0;
}
