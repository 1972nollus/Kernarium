// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_ordered_test_support.hpp"

#include <array>
#include <cstdio>

namespace ordered = arm64_simd_ordered_test;
namespace engine = uml::emulator::tests;

namespace {

void check_success(engine::EngineHarness &harness,
	const ordered::SimdVariant &variant, int offset, unsigned rn,
	unsigned rt, std::uint64_t seed)
{
	std::vector<std::byte> initial_memory(ordered::page_size);
	ordered::fill_page(initial_memory, static_cast<unsigned>(seed));
	auto expected_memory = initial_memory;
	auto initial = ordered::seeded_state(seed);
	const auto base = ordered::address -
		(variant.kind == ordered::SimdKind::unscaled ?
			static_cast<std::uint64_t>(
				static_cast<std::int64_t>(offset)) : 0U);
	ordered::set_base(initial, rn, base);
	auto expected = initial;
	const auto memory_offset =
		static_cast<std::size_t>(ordered::address - ordered::page_base);
	if (variant.load) {
		if (variant.kind == ordered::SimdKind::unscaled) {
			expected.vectors[rt].bytes.fill(std::byte{});
			for (std::size_t index = 0; index < variant.size; ++index)
				expected.vectors[rt].bytes[index] =
					initial_memory[memory_offset + index];
		} else {
			const auto lane_offset = variant.upper ? 8U : 0U;
			for (std::size_t index = 0; index < 8U; ++index)
				expected.vectors[rt].bytes[lane_offset + index] =
					initial_memory[memory_offset + index];
		}
	} else {
		const auto lane_offset =
			variant.kind == ordered::SimdKind::lane && variant.upper ?
				8U : 0U;
		for (std::size_t index = 0; index < variant.size; ++index)
			expected_memory[memory_offset + index] =
				initial.vectors[rt].bytes[lane_offset + index];
	}
	const std::array regions{
		engine::MemoryRegion{
			.address = ordered::page_base,
			.initial = std::span<const std::byte>{initial_memory},
			.expected = std::span<const std::byte>{expected_memory},
		},
	};
	char name[136];
	std::snprintf(name, sizeof(name),
		"%.*s behavior offset=%d rn=%u rt=%u",
		static_cast<int>(variant.name.size()), variant.name.data(),
		offset, rn, rt);
	harness.check({
		.name = name,
		.instruction =
			ordered::simd_instruction(variant, offset, rn, rt),
		.initial = initial,
		.expected = expected,
		.native = ordered::native(variant),
		.memory = std::span<const engine::MemoryRegion>{regions},
	});
}

void every_immediate_and_register(engine::EngineHarness &harness)
{
	std::uint64_t seed = 0xb100'0000'0000'0000ULL;
	for (const auto &variant : ordered::simd_unscaled_variants) {
		for (unsigned imm9 = 0; imm9 < 512U; ++imm9) {
			const auto offset = static_cast<int>(
				static_cast<std::int16_t>(imm9 << 7) >> 7);
			check_success(harness, variant, offset, 2, 3, seed++);
		}
		for (unsigned rn = 0; rn < 32U; ++rn)
			check_success(harness, variant, 0, rn, 3, seed++);
		for (unsigned rt = 0; rt < 32U; ++rt)
			check_success(harness, variant, 0, 2, rt, seed++);
	}
	for (const auto &variant : ordered::simd_lane_variants) {
		for (unsigned rn = 0; rn < 32U; ++rn)
			check_success(harness, variant, 0, rn, 3, seed++);
		for (unsigned rt = 0; rt < 32U; ++rt)
			check_success(harness, variant, 0, 2, rt, seed++);
	}
}

template <std::size_t Size>
void store_variants_invalidate_reservations(
	engine::EngineHarness &harness,
	const std::array<ordered::SimdVariant, Size> &variants,
	std::uint64_t &seed)
{
	for (const auto &variant : variants) {
		if (variant.load)
			continue;
		std::vector<std::byte> initial_memory(ordered::page_size);
		ordered::fill_page(initial_memory, static_cast<unsigned>(seed));
		auto expected_memory = initial_memory;
		auto initial = ordered::seeded_state(seed++);
		initial.registers[2] = ordered::address;
		initial.registers[3] = ordered::address;
		const auto monitored = ordered::load_integer(
			initial_memory, ordered::address - ordered::page_base, 8);
		const auto lane_offset =
			variant.kind == ordered::SimdKind::lane && variant.upper ?
				8U : 0U;
		for (std::size_t index = 0; index < variant.size; ++index)
			expected_memory[ordered::address - ordered::page_base + index] =
				initial.vectors[4].bytes[lane_offset + index];

		auto expected = initial;
		expected.registers[5] = monitored;
		expected.registers[6] = 1;
		expected.exclusive_address = ~std::uint64_t{0};
		expected.exclusive_key = 0;
		expected.exclusive_size = 0;
		expected.pc = engine::engine_test_code_base + 16;

		const std::array code{
			engine::ProgramWord{0,
				ordered::scalar_exclusive(3, true, 2, 5)},
			engine::ProgramWord{4,
				ordered::simd_instruction(variant, 0, 3, 4)},
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
		});
	}
}

void stores_invalidate_reservations(engine::EngineHarness &harness)
{
	std::uint64_t seed = 0xb200'0000'0000'0000ULL;
	store_variants_invalidate_reservations(
		harness, ordered::simd_unscaled_variants, seed);
	store_variants_invalidate_reservations(
		harness, ordered::simd_lane_variants, seed);
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	every_immediate_and_register(harness);
	stores_invalidate_reservations(harness);
	harness.print_summary("arm64-simd-ordered-behavior");
	return 0;
}
