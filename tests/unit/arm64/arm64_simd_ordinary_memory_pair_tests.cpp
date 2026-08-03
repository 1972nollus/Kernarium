// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_ordinary_memory_test_support.hpp"

#include <cstdio>

namespace memory = arm64_simd_ordinary_memory_test;
namespace engine = uml::emulator::tests;

namespace {

[[nodiscard]] constexpr bool writeback(memory::PairMode mode)
{
	return mode == memory::PairMode::post_index ||
		mode == memory::PairMode::pre_index;
}

void check_pair(
	engine::EngineHarness &harness, std::string_view name, bool load,
	memory::PairMode mode, unsigned imm7, unsigned rn,
	unsigned rt, unsigned rt2, std::uint64_t base,
	std::span<const std::byte> initial_memory, std::uint64_t seed)
{
	auto initial = memory::seeded_state(seed);
	memory::write_base(initial, rn, base);
	auto expected = initial;
	auto expected_memory =
		std::vector<std::byte>(initial_memory.begin(), initial_memory.end());
	const auto adjusted = base + static_cast<std::uint64_t>(
		memory::signed_imm7(imm7) * 16);
	const auto address =
		mode == memory::PairMode::post_index ? base : adjusted;
	memory::apply_pair_oracle(initial, expected, initial_memory,
		expected_memory,
		static_cast<std::size_t>(address - memory::data_base),
		load, rt, rt2);
	if (writeback(mode))
		memory::write_base(expected, rn, adjusted);
	const std::array regions{
		engine::MemoryRegion{
			.address = memory::data_base,
			.initial = initial_memory,
			.expected = std::span<const std::byte>{expected_memory},
		},
	};
	memory::check(harness, name,
		memory::pair_instruction(load, mode, imm7, rn, rt, rt2),
		initial, expected, memory::pair_native(), regions);
}

void every_mode_direction_and_immediate(engine::EngineHarness &harness,
	std::span<const std::byte> page, std::uint64_t &seed)
{
	const auto base = memory::data_base + memory::page_size / 2U;
	for (unsigned mode = 0; mode < 4U; ++mode)
		for (const bool load : {false, true})
			for (unsigned imm7 = 0; imm7 < 128U; ++imm7) {
				char name[144];
				std::snprintf(name, sizeof(name),
					"%s mode=%u every imm7=%u",
					load ? "load-pair" : "store-pair",
					mode, imm7);
				check_pair(harness, name, load,
					static_cast<memory::PairMode>(mode),
					imm7, 20, 3, 17, base, page, seed++);
			}
}

void every_register_number(engine::EngineHarness &harness,
	std::span<const std::byte> page, std::uint64_t &seed)
{
	const auto base = memory::data_base + memory::page_size / 2U;
	for (unsigned mode = 0; mode < 4U; ++mode)
		for (const bool load : {false, true}) {
			for (unsigned rt = 0; rt < 32U; ++rt) {
				const unsigned rt2 = (rt + 13U) & 31U;
				char name[144];
				std::snprintf(name, sizeof(name),
					"%s mode=%u rt=%u rt2=%u",
					load ? "load-pair" : "store-pair",
					mode, rt, rt2);
				check_pair(harness, name, load,
					static_cast<memory::PairMode>(mode),
					2, 20, rt, rt2, base, page, seed++);
			}
			for (unsigned rn = 0; rn < 32U; ++rn) {
				char name[136];
				std::snprintf(name, sizeof(name),
					"%s mode=%u every base rn=%u",
					load ? "load-pair" : "store-pair",
					mode, rn);
				check_pair(harness, name, load,
					static_cast<memory::PairMode>(mode),
					3, rn, rn, (rn + 1U) & 31U,
					base, page, seed++);
			}
		}
}

void duplicate_store_sources(engine::EngineHarness &harness,
	std::span<const std::byte> page, std::uint64_t &seed)
{
	const auto base = memory::data_base + memory::page_size / 2U;
	for (unsigned mode = 0; mode < 4U; ++mode)
		for (unsigned source = 0; source < 32U; ++source) {
			char name[128];
			std::snprintf(name, sizeof(name),
				"store-pair duplicate source=%u mode=%u",
				source, mode);
			check_pair(harness, name, false,
				static_cast<memory::PairMode>(mode), 1,
				31, source, source, base, page, seed++);
		}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	std::vector<std::byte> page(memory::page_size);
	memory::fill_memory(page, 0xb5);
	std::uint64_t seed = 0xa000;
	every_mode_direction_and_immediate(harness, page, seed);
	every_register_number(harness, page, seed);
	duplicate_store_sources(harness, page, seed);
	harness.print_summary("arm64-simd-ordinary-memory-pair");
}
