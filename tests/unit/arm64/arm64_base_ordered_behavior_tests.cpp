// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_ordered_memory_test_support.hpp"

#include <array>
#include <cstdio>

namespace ordered = arm64_ordered_memory_test;
namespace engine = uml::emulator::tests;

namespace {

void check_success(engine::EngineHarness &harness,
	const ordered::ScalarVariant &variant, int offset, unsigned rn,
	unsigned rt, std::uint64_t seed)
{
	const auto size = std::size_t{1} << variant.size_code;
	std::vector<std::byte> initial_memory(ordered::page_size);
	ordered::fill_page(initial_memory,
		static_cast<unsigned>(seed ^ (seed >> 32)));
	auto expected_memory = initial_memory;

	auto initial = ordered::seeded_state(seed);
	const auto base = ordered::scalar_base_for_target(
		variant, ordered::address, offset);
	ordered::set_base(initial, rn, base);
	auto expected = initial;
	const auto memory_offset =
		static_cast<std::size_t>(ordered::address - ordered::page_base);
	if (variant.load) {
		const auto raw = ordered::load_integer(
			initial_memory, memory_offset, size);
		ordered::write_gpr(expected, rt,
			ordered::scalar_load_result(variant, raw));
	} else {
		const auto source = ordered::read_gpr(initial, rt);
		ordered::store_integer(expected_memory, memory_offset, size, source);
	}
	if (variant.instruction_class == ordered::ScalarClass::writeback) {
		const auto adjusted = variant.load ? base + size : base - size;
		const bool suppress = variant.load && rn != 31U && rn == rt;
		if (!suppress)
			ordered::write_base(expected, rn, adjusted);
	}

	const std::array regions{
		engine::MemoryRegion{
			.address = ordered::page_base,
			.initial = std::span<const std::byte>{initial_memory},
			.expected = std::span<const std::byte>{expected_memory},
		},
	};
	char name[128];
	std::snprintf(name, sizeof(name),
		"%.*s behavior offset=%d rn=%u rt=%u",
		static_cast<int>(variant.name.size()), variant.name.data(),
		offset, rn, rt);
	harness.check({
		.name = name,
		.instruction = ordered::scalar_instruction(
			variant, offset, rn, rt),
		.initial = initial,
		.expected = expected,
		.native = ordered::native(variant),
		.memory = std::span<const engine::MemoryRegion>{regions},
	});
}

void every_register_and_alias(engine::EngineHarness &harness)
{
	std::uint64_t seed = 0x5100'0000'0000'0000ULL;
	for (const auto &variant : ordered::scalar_variants) {
		for (unsigned rn = 0; rn < 32U; ++rn)
			check_success(harness, variant, 0, rn, 7, seed++);
		for (unsigned rt = 0; rt < 32U; ++rt)
			check_success(harness, variant, 0, 6, rt, seed++);
	}
}

void every_unscaled_immediate(engine::EngineHarness &harness)
{
	std::uint64_t seed = 0x5200'0000'0000'0000ULL;
	for (const auto &variant : ordered::scalar_variants) {
		if (variant.instruction_class !=
		    ordered::ScalarClass::rcpc_unscaled)
			continue;
		for (unsigned imm9 = 0; imm9 < 512U; ++imm9) {
			const auto offset = static_cast<int>(
				static_cast<std::int16_t>(imm9 << 7) >> 7);
			check_success(harness, variant, offset, 2, 3, seed++);
		}
	}
}

void stores_invalidate_reservations(engine::EngineHarness &harness)
{
	std::uint64_t seed = 0x5300'0000'0000'0000ULL;
	for (const auto &variant : ordered::scalar_variants) {
		if (variant.load)
			continue;
		const auto size = std::size_t{1} << variant.size_code;
		std::vector<std::byte> initial_memory(ordered::page_size);
		ordered::fill_page(initial_memory,
			static_cast<unsigned>(seed ^ (seed >> 32)));
		auto expected_memory = initial_memory;
		auto initial = ordered::seeded_state(seed++);
		initial.registers[2] = ordered::address;
		initial.registers[3] =
			ordered::scalar_base_for_target(variant, ordered::address, 0);
		const auto monitored = ordered::load_integer(
			initial_memory, ordered::address - ordered::page_base, 8);
		ordered::store_integer(expected_memory,
			ordered::address - ordered::page_base, size,
			initial.registers[4]);

		auto expected = initial;
		expected.registers[5] = monitored;
		expected.registers[6] = 1;
		if (variant.instruction_class == ordered::ScalarClass::writeback)
			expected.registers[3] = ordered::address;
		expected.exclusive_address = ~std::uint64_t{0};
		expected.exclusive_key = 0;
		expected.exclusive_size = 0;
		expected.pc = engine::engine_test_code_base + 16;

		const std::array code{
			engine::ProgramWord{0,
				ordered::scalar_exclusive(3, true, 2, 5)},
			engine::ProgramWord{4, ordered::scalar_instruction(
				variant, 0, 3, 4)},
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
	every_register_and_alias(harness);
	every_unscaled_immediate(harness);
	stores_invalidate_reservations(harness);
	harness.print_summary("arm64-base-ordered-behavior");
	return 0;
}
