// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_literal_load_test_support.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

namespace engine_tests = uml::backend::tests;
namespace literal_test = arm64_literal_load_test;

namespace {

constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

void check_mapped_load(
	engine_tests::EngineHarness &harness, std::string_view name,
	const literal_test::Variant &variant, unsigned destination,
	std::uint64_t target, std::uint64_t seed)
{
	const auto mapping_base = literal_test::page_base(target);
	const auto mapping_size =
		target + variant.size > mapping_base + literal_test::page_size ?
			2U * literal_test::page_size : literal_test::page_size;
	std::vector<std::byte> memory(mapping_size);
	literal_test::fill_memory(memory,
		static_cast<unsigned>(seed ^ destination ^ variant.operation));
	auto initial = literal_test::seeded_state(seed);
	auto expected = initial;
	literal_test::apply_load(expected, variant, destination, memory,
		static_cast<std::size_t>(target - mapping_base));
	const std::array regions{
		engine_tests::MemoryRegion{
			.address = mapping_base,
			.initial = std::span<const std::byte>{memory},
		},
	};
	harness.check({
		.name = name,
		.instruction = literal_test::literal(
			variant.vector, variant.operation,
			literal_test::immediate_for_target(
				literal_test::code_base, target),
			destination),
		.initial = initial,
		.expected = expected,
		.native = literal_test::native(variant),
		.memory = std::span<const engine_tests::MemoryRegion>{regions},
	});
}

void every_destination_and_width(engine_tests::EngineHarness &harness)
{
	const auto target = literal_test::code_base +
		3U * literal_test::page_size + 0x184U;
	for (unsigned variant_index = 0;
	     variant_index < literal_test::load_variants.size();
	     ++variant_index) {
		const auto &variant = literal_test::load_variants[variant_index];
		for (unsigned destination = 0; destination < 32;
		     ++destination) {
			char name[96];
			std::snprintf(name, sizeof(name), "%.*s destination=%u",
				static_cast<int>(variant.name.size()),
				variant.name.data(), destination);
			check_mapped_load(harness, name, variant, destination, target,
				0x1000'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(variant_index) << 16) |
					destination);
		}
	}
}

void signed_and_zero_extension(engine_tests::EngineHarness &harness)
{
	const auto target = literal_test::code_base -
		5U * literal_test::page_size + 0x240U;
	const auto mapping_base = literal_test::page_base(target);
	std::vector<std::byte> memory(literal_test::page_size);
	literal_test::fill_memory(memory, 0x51U);
	const auto offset = static_cast<std::size_t>(target - mapping_base);
	constexpr std::uint32_t negative_word = 0x8000'0041U;
	std::memcpy(memory.data() + offset, &negative_word,
		sizeof(negative_word));
	constexpr std::uint64_t doubleword = 0xfedc'ba98'7654'3210ULL;
	std::memcpy(memory.data() + offset + 8U, &doubleword,
		sizeof(doubleword));

	const std::array regions{
		engine_tests::MemoryRegion{
			.address = mapping_base,
			.initial = std::span<const std::byte>{memory},
		},
	};
	for (const auto [variant_index, adjustment] :
	     {std::array{0U, 0U}, std::array{1U, 8U},
	      std::array{2U, 0U}}) {
		const auto &variant =
			literal_test::load_variants[variant_index];
		const auto address = target + adjustment;
		auto initial = literal_test::seeded_state(
			0x2000'0000'0000'0000ULL | variant_index);
		initial.registers[9] = 0xaaaa'bbbb'cccc'ddddULL;
		auto expected = initial;
		literal_test::apply_load(expected, variant, 9, memory,
			static_cast<std::size_t>(address - mapping_base));
		char name[96];
		std::snprintf(name, sizeof(name), "%.*s extension semantics",
			static_cast<int>(variant.name.size()),
			variant.name.data());
		harness.check({
			.name = name,
			.instruction = literal_test::literal(
				variant.vector, variant.operation,
				literal_test::immediate_for_target(
					literal_test::code_base, address),
				9),
			.initial = initial,
			.expected = expected,
			.native = literal_test::native(variant),
			.memory =
				std::span<const engine_tests::MemoryRegion>{regions},
		});
	}
}

void pc_relative_boundaries(engine_tests::EngineHarness &harness)
{
	constexpr std::array immediate_boundaries{
		0x40000U, 0x40001U, 0x3fffeU, 0x3ffffU};
	for (unsigned variant_index = 0;
	     variant_index < literal_test::load_variants.size();
	     ++variant_index) {
		const auto &variant = literal_test::load_variants[variant_index];
		for (const auto imm19 : immediate_boundaries) {
			const auto address =
				literal_test::target(literal_test::code_base, imm19);
			char name[112];
			std::snprintf(name, sizeof(name),
				"%.*s PC-relative boundary imm19=%#x",
				static_cast<int>(variant.name.size()),
				variant.name.data(), imm19);
			check_mapped_load(harness, name, variant, 13, address,
				0x3000'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(variant_index) << 24) |
					imm19);
		}
	}
}

void instruction_pc_not_block_base(engine_tests::EngineHarness &harness)
{
	constexpr std::size_t entry_offset = 0x80;
	const auto instruction_pc = literal_test::code_base + entry_offset;
	const auto target = literal_test::code_base +
		6U * literal_test::page_size + 0x2c0U;
	const auto mapping_base = literal_test::page_base(target);
	std::vector<std::byte> memory(literal_test::page_size);
	literal_test::fill_memory(memory, 0x71U);
	const std::array regions{
		engine_tests::MemoryRegion{
			.address = mapping_base,
			.initial = std::span<const std::byte>{memory},
		},
	};
	for (unsigned variant_index = 0;
	     variant_index < literal_test::load_variants.size();
	     ++variant_index) {
		const auto &variant = literal_test::load_variants[variant_index];
		const std::array code{
			engine_tests::ProgramWord{
				.offset = entry_offset,
				.instruction = literal_test::literal(
					variant.vector, variant.operation,
					literal_test::immediate_for_target(
						instruction_pc, target),
					21),
			},
			engine_tests::ProgramWord{
				.offset = entry_offset + sizeof(std::uint32_t),
				.instruction = terminal_system_call,
			},
		};
		auto initial = literal_test::seeded_state(
			0x4000'0000'0000'0000ULL | variant_index);
		auto expected = initial;
		literal_test::apply_load(expected, variant, 21, memory,
			static_cast<std::size_t>(target - mapping_base));
		expected.pc = instruction_pc + 2U * sizeof(std::uint32_t);
		char name[104];
		std::snprintf(name, sizeof(name),
			"%.*s instruction PC differs from block base",
			static_cast<int>(variant.name.size()),
			variant.name.data());
		harness.check_program({
			.name = name,
			.code = std::span<const engine_tests::ProgramWord>{code},
			.entry_offset = entry_offset,
			.initial = initial,
			.expected = expected,
			.native = literal_test::native(variant),
			.memory =
				std::span<const engine_tests::MemoryRegion>{regions},
		});
	}
}

void loads_from_code_page(engine_tests::EngineHarness &harness)
{
	for (unsigned variant_index = 0;
	     variant_index < literal_test::load_variants.size();
	     ++variant_index) {
		const auto &variant = literal_test::load_variants[variant_index];
		const auto instruction = literal_test::literal(
			variant.vector, variant.operation, 0, 23);
		std::array<std::byte, 16> code_bytes{};
		std::memcpy(code_bytes.data(), &instruction, sizeof(instruction));
		std::memcpy(code_bytes.data() + sizeof(instruction),
			&terminal_system_call, sizeof(terminal_system_call));
		auto initial = literal_test::seeded_state(
			0x5000'0000'0000'0000ULL | variant_index);
		auto expected = initial;
		literal_test::apply_load(
			expected, variant, 23, code_bytes, 0);
		char name[88];
		std::snprintf(name, sizeof(name), "%.*s reads code page",
			static_cast<int>(variant.name.size()),
			variant.name.data());
		harness.check_one(name, instruction, initial, expected,
			literal_test::native(variant));
	}
}

} // namespace

int main()
{
	engine_tests::EngineHarness harness;
	every_destination_and_width(harness);
	signed_and_zero_extension(harness);
	pc_relative_boundaries(harness);
	instruction_pc_not_block_base(harness);
	loads_from_code_page(harness);
	harness.print_summary("arm64-literal-load-behavior");
	return 0;
}
