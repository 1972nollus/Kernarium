// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_ordinary_memory_test_support.hpp"

#include <array>
#include <cstdio>

namespace memory = arm64_simd_ordinary_memory_test;
namespace engine = uml::backend::tests;

namespace {

void check_single(
	engine::EngineHarness &harness, std::string_view name,
	const memory::SingleVariant &variant, std::uint32_t instruction,
	arm64_simd_ordinary_memory_test::arm64::CpuState initial,
	std::uint64_t address, bool writeback, std::uint64_t adjusted,
	unsigned rn, unsigned rt, std::span<const std::byte> initial_memory)
{
	auto expected = initial;
	auto expected_memory =
		std::vector<std::byte>(initial_memory.begin(), initial_memory.end());
	memory::apply_single_oracle(initial, expected, initial_memory,
		expected_memory,
		static_cast<std::size_t>(address - memory::data_base), variant, rt);
	if (writeback)
		memory::write_base(expected, rn, adjusted);
	const std::array regions{
		engine::MemoryRegion{
			.address = memory::data_base,
			.initial = initial_memory,
			.expected = std::span<const std::byte>{expected_memory},
		},
	};
	memory::check(harness, name, instruction, initial, expected,
		memory::single_native(), regions);
}

void every_signed_immediate(engine::EngineHarness &harness,
	std::span<const std::byte> page, std::uint64_t &seed)
{
	const auto base = memory::data_base + memory::page_size / 2U;
	for (const auto &variant : memory::single_variants)
		for (const auto mode : {
		     memory::ImmediateMode::unscaled,
		     memory::ImmediateMode::post_index,
		     memory::ImmediateMode::pre_index})
			for (unsigned imm9 = 0; imm9 < 512U; ++imm9) {
				auto initial = memory::seeded_state(seed++);
				constexpr unsigned rn = 2;
				constexpr unsigned rt = 29;
				memory::write_base(initial, rn, base);
				const auto adjusted =
					base + static_cast<std::uint64_t>(
						memory::signed_imm9(imm9));
				const bool post =
					mode == memory::ImmediateMode::post_index;
				const auto address = post ? base : adjusted;
				char name[144];
				std::snprintf(name, sizeof(name),
					"%.*s signed mode=%u imm9=%u",
					static_cast<int>(variant.name.size()),
					variant.name.data(),
					static_cast<unsigned>(mode), imm9);
				check_single(harness, name, variant,
					memory::immediate_instruction(
						variant.size_code, variant.operation,
						imm9, mode, rn, rt),
					initial, address,
					mode != memory::ImmediateMode::unscaled,
					adjusted, rn, rt, page);
			}
}

void unsigned_offset_bit_matrix(engine::EngineHarness &harness,
	std::span<const std::byte> memory_bytes, std::uint64_t &seed)
{
	constexpr std::array immediates{
		0U, 1U, 2U, 3U, 7U, 15U, 31U, 63U, 127U, 255U,
		511U, 1023U, 2047U, 2048U, 4095U,
	};
	for (const auto &variant : memory::single_variants)
		for (const auto imm12 : immediates) {
			auto initial = memory::seeded_state(seed++);
			constexpr unsigned rn = 3;
			constexpr unsigned rt = 31;
			memory::write_base(initial, rn, memory::data_base);
			const auto address =
				memory::data_base + imm12 * variant.size;
			char name[136];
			std::snprintf(name, sizeof(name),
				"%.*s unsigned imm12=%u",
				static_cast<int>(variant.name.size()),
				variant.name.data(), imm12);
			check_single(harness, name, variant,
				memory::unsigned_instruction(
					variant.size_code, variant.operation,
					imm12, rn, rt),
				initial, address, false, memory::data_base,
				rn, rt, memory_bytes);
		}
}

void every_base_transfer_and_alias(engine::EngineHarness &harness,
	std::span<const std::byte> page, std::uint64_t &seed)
{
	const auto base = memory::data_base + memory::page_size / 2U;
	for (const auto &variant : memory::single_variants) {
		for (unsigned rn = 0; rn < 32U; ++rn) {
			const unsigned rt = rn;
			auto initial = memory::seeded_state(seed++);
			memory::write_base(initial, rn, base);
			char name[144];
			std::snprintf(name, sizeof(name),
				"%.*s same-number base/transfer=%u",
				static_cast<int>(variant.name.size()),
				variant.name.data(), rn);
			check_single(harness, name, variant,
				memory::immediate_instruction(
					variant.size_code, variant.operation, 3,
					memory::ImmediateMode::pre_index, rn, rt),
				initial, base + 3U, true, base + 3U,
				rn, rt, page);
		}
		for (unsigned rt = 0; rt < 32U; ++rt) {
			auto initial = memory::seeded_state(seed++);
			constexpr unsigned rn = 20;
			memory::write_base(initial, rn, base);
			char name[128];
			std::snprintf(name, sizeof(name),
				"%.*s every transfer rt=%u",
				static_cast<int>(variant.name.size()),
				variant.name.data(), rt);
			check_single(harness, name, variant,
				memory::unsigned_instruction(
					variant.size_code, variant.operation,
					1, rn, rt),
				initial, base + variant.size, false, base,
				rn, rt, page);
		}
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	std::vector<std::byte> page(memory::page_size);
	memory::fill_memory(page, 0x71);
	constexpr std::size_t unsigned_pages = 17;
	std::vector<std::byte> unsigned_memory(
		unsigned_pages * memory::page_size);
	memory::fill_memory(unsigned_memory, 0x93);
	std::uint64_t seed = 0x4000;
	every_signed_immediate(harness, page, seed);
	unsigned_offset_bit_matrix(harness, unsigned_memory, seed);
	every_base_transfer_and_alias(harness, page, seed);
	harness.print_summary("arm64-simd-ordinary-memory-immediate");
}
