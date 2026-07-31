// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_ordinary_memory_test_support.hpp"

#include <array>
#include <cstdio>

namespace memory = arm64_simd_ordinary_memory_test;
namespace engine = uml::backend::tests;

namespace {

void check_register(
	engine::EngineHarness &harness, std::string_view name,
	const memory::SingleVariant &variant, unsigned rm, unsigned option,
	bool scaled, unsigned rn, unsigned rt, std::uint64_t raw_offset,
	std::uint64_t base, std::uint64_t address,
	std::span<const std::byte> initial_memory, std::uint64_t seed)
{
	auto initial = memory::seeded_state(seed);
	memory::write_base(initial, rn, base);
	if (rm < 31U)
		initial.registers[rm] = raw_offset;
	auto expected = initial;
	auto expected_memory =
		std::vector<std::byte>(initial_memory.begin(), initial_memory.end());
	memory::apply_single_oracle(initial, expected, initial_memory,
		expected_memory,
		static_cast<std::size_t>(address - memory::data_base), variant, rt);
	const std::array regions{
		engine::MemoryRegion{
			.address = memory::data_base,
			.initial = initial_memory,
			.expected = std::span<const std::byte>{expected_memory},
		},
	};
	memory::check(harness, name,
		memory::register_instruction(
			variant.size_code, variant.operation, rm, option,
			scaled, rn, rt),
		initial, expected, memory::single_native(), regions);
}

void every_extension_and_boundary_value(engine::EngineHarness &harness,
	std::span<const std::byte> page, std::uint64_t &seed)
{
	constexpr std::array raw_values{
		std::uint64_t{0}, std::uint64_t{1},
		std::uint64_t{0x7fff'ffffU}, std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		~std::uint64_t{0},
	};
	const auto target = memory::data_base + memory::page_size / 2U;
	for (const auto &variant : memory::single_variants)
		for (const unsigned option : {2U, 3U, 6U, 7U})
			for (const bool scaled : {false, true})
				for (const auto raw : raw_values) {
					const unsigned shift = scaled ?
						static_cast<unsigned>(
							variant.size == 16U ? 4U :
							variant.size_code) :
						0U;
					const auto offset =
						memory::extend_offset(raw, option, shift);
					const auto base = target - offset;
					char name[160];
					std::snprintf(name, sizeof(name),
						"%.*s option=%u scaled=%u raw=%#llx",
						static_cast<int>(variant.name.size()),
						variant.name.data(), option,
						static_cast<unsigned>(scaled),
						static_cast<unsigned long long>(raw));
					check_register(harness, name, variant, 5, option,
						scaled, 20, 29, raw, base, target,
						page, seed++);
				}
}

void every_offset_register(engine::EngineHarness &harness,
	std::span<const std::byte> page, std::uint64_t &seed)
{
	const auto target = memory::data_base + memory::page_size / 2U;
	for (const auto &variant : memory::single_variants)
		for (unsigned rm = 0; rm < 32U; ++rm) {
			const unsigned rn = rm == 20U ? 21U : 20U;
			const auto raw = rm == 31U ? 0U :
				static_cast<std::uint64_t>((rm % 7U) + 1U);
			const auto base = target - raw;
			char name[128];
			std::snprintf(name, sizeof(name),
				"%.*s every offset rm=%u",
				static_cast<int>(variant.name.size()),
				variant.name.data(), rm);
			check_register(harness, name, variant, rm, 3, false,
				rn, 31, raw, base, target, page, seed++);
		}
}

void register_aliases_and_stack(engine::EngineHarness &harness,
	std::span<const std::byte> page, std::uint64_t &seed)
{
	const auto target =
		(memory::data_base + memory::page_size / 2U) & ~std::uint64_t{15};
	for (const auto &variant : memory::single_variants) {
		/* Rn==Rm reads the one architectural X register for both roles. */
		const auto aliased = target / 2U;
		char alias_name[128];
		std::snprintf(alias_name, sizeof(alias_name),
			"%.*s rn-rm alias",
			static_cast<int>(variant.name.size()), variant.name.data());
		check_register(harness, alias_name, variant, 6, 3, false,
			6, 6, aliased, aliased, target, page, seed++);

		char stack_name[128];
		std::snprintf(stack_name, sizeof(stack_name),
			"%.*s sp-base xzr-offset",
			static_cast<int>(variant.name.size()), variant.name.data());
		check_register(harness, stack_name, variant, 31, 3, true,
			31, 31, 0, target, target, page, seed++);
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	std::vector<std::byte> page(memory::page_size);
	memory::fill_memory(page, 0x37);
	std::uint64_t seed = 0x7000;
	every_extension_and_boundary_value(harness, page, seed);
	every_offset_register(harness, page, seed);
	register_aliases_and_stack(harness, page, seed);
	harness.print_summary("arm64-simd-ordinary-memory-register");
}
