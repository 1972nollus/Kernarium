// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_literal_load_test_support.hpp"

#include <array>
#include <cstdio>
#include <span>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;
namespace literal_test = arm64_literal_load_test;

namespace {

void unmapped_and_protected_faults(engine_tests::EngineHarness &harness)
{
	for (unsigned variant_index = 0;
	     variant_index < literal_test::load_variants.size();
	     ++variant_index) {
		const auto &variant = literal_test::load_variants[variant_index];
		for (const unsigned destination : {7U, 31U})
			for (const bool negative : {false, true}) {
				const auto address = negative ?
					literal_test::code_base -
						8U * literal_test::page_size + 0x180U :
					literal_test::code_base +
						8U * literal_test::page_size + 0x180U;
				auto initial = literal_test::seeded_state(
					0x1000'0000'0000'0000ULL |
					(static_cast<std::uint64_t>(variant_index) << 12) |
					(destination << 1) | negative);
				char name[112];
				std::snprintf(name, sizeof(name),
					"%.*s unmapped rt=%u direction=%s",
					static_cast<int>(variant.name.size()),
					variant.name.data(), destination,
					negative ? "negative" : "positive");
				harness.check({
					.name = name,
					.instruction = literal_test::literal(
						variant.vector, variant.operation,
						literal_test::immediate_for_target(
							literal_test::code_base, address),
						destination),
					.initial = initial,
					.expected = initial,
					.event = engine_tests::ExpectedEvent::memory_fault,
					.native = literal_test::native(variant),
					.fault = {
						.address = address,
						.access = arm64::MemoryAccess::read,
					},
					.compare_fault = true,
				});
			}

		const auto address = literal_test::code_base +
			9U * literal_test::page_size + 0x240U;
		const auto mapping_base = literal_test::page_base(address);
		std::vector<std::byte> memory(literal_test::page_size);
		literal_test::fill_memory(memory, variant_index + 0x40U);
		const std::array regions{
			engine_tests::MemoryRegion{
				.address = mapping_base,
				.initial = std::span<const std::byte>{memory},
				.readable = false,
				.writable = true,
			},
		};
		auto initial = literal_test::seeded_state(
			0x2000'0000'0000'0000ULL | variant_index);
		char name[88];
		std::snprintf(name, sizeof(name), "%.*s protected page",
			static_cast<int>(variant.name.size()),
			variant.name.data());
		harness.check({
			.name = name,
			.instruction = literal_test::literal(
				variant.vector, variant.operation,
				literal_test::immediate_for_target(
					literal_test::code_base, address),
				11),
			.initial = initial,
			.expected = initial,
			.event = engine_tests::ExpectedEvent::memory_fault,
			.native = literal_test::native(variant),
			.fault = {
				.address = address,
				.access = arm64::MemoryAccess::read,
			},
			.compare_fault = true,
			.memory =
				std::span<const engine_tests::MemoryRegion>{regions},
		});
	}
}

void cross_page_success(engine_tests::EngineHarness &harness)
{
	const auto mapping_base =
		literal_test::code_base + 10U * literal_test::page_size;
	const auto boundary = mapping_base + literal_test::page_size;
	for (unsigned variant_index = 0;
	     variant_index < literal_test::load_variants.size();
	     ++variant_index) {
		const auto &variant = literal_test::load_variants[variant_index];
		if (variant.size == 4U)
			continue;
		for (std::size_t first_page_bytes = 4U;
		     first_page_bytes < variant.size;
		     first_page_bytes += 4U) {
			const auto address = boundary - first_page_bytes;
			std::vector<std::byte> memory(
				2U * literal_test::page_size);
			literal_test::fill_memory(memory,
				0x60U + variant_index * 7U +
					static_cast<unsigned>(first_page_bytes));
			auto initial = literal_test::seeded_state(
				0x3000'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(variant_index) << 8) |
				first_page_bytes);
			auto expected = initial;
			literal_test::apply_load(expected, variant, 19, memory,
				static_cast<std::size_t>(address - mapping_base));
			const std::array regions{
				engine_tests::MemoryRegion{
					.address = mapping_base,
					.initial =
						std::span<const std::byte>{memory},
				},
			};
			char name[104];
			std::snprintf(name, sizeof(name),
				"%.*s cross-page split=%zu",
				static_cast<int>(variant.name.size()),
				variant.name.data(), first_page_bytes);
			harness.check({
				.name = name,
				.instruction = literal_test::literal(
					variant.vector, variant.operation,
					literal_test::immediate_for_target(
						literal_test::code_base, address),
					19),
				.initial = initial,
				.expected = expected,
				.native = literal_test::native(variant),
				.memory =
					std::span<const engine_tests::MemoryRegion>{
						regions},
			});
		}
	}
}

void cross_page_fault_atomicity(engine_tests::EngineHarness &harness)
{
	const auto mapping_base =
		literal_test::code_base - 10U * literal_test::page_size;
	const auto boundary = mapping_base + literal_test::page_size;
	for (unsigned variant_index = 0;
	     variant_index < literal_test::load_variants.size();
	     ++variant_index) {
		const auto &variant = literal_test::load_variants[variant_index];
		if (variant.size == 4U)
			continue;
		for (std::size_t first_page_bytes = 4U;
		     first_page_bytes < variant.size;
		     first_page_bytes += 4U) {
			const auto address = boundary - first_page_bytes;
			std::vector<std::byte> memory(literal_test::page_size);
			literal_test::fill_memory(memory,
				0x90U + variant_index * 5U +
					static_cast<unsigned>(first_page_bytes));
			const std::array regions{
				engine_tests::MemoryRegion{
					.address = mapping_base,
					.initial =
						std::span<const std::byte>{memory},
				},
			};
			auto initial = literal_test::seeded_state(
				0x4000'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(variant_index) << 8) |
				first_page_bytes);
			char name[112];
			std::snprintf(name, sizeof(name),
				"%.*s cross-page fault split=%zu",
				static_cast<int>(variant.name.size()),
				variant.name.data(), first_page_bytes);
			harness.check({
				.name = name,
				.instruction = literal_test::literal(
					variant.vector, variant.operation,
					literal_test::immediate_for_target(
						literal_test::code_base, address),
					19),
				.initial = initial,
				.expected = initial,
				.event = engine_tests::ExpectedEvent::memory_fault,
				.native = literal_test::native(variant),
				.fault = {
					.address = boundary,
					.access = arm64::MemoryAccess::read,
				},
				.compare_fault = true,
				.memory =
					std::span<const engine_tests::MemoryRegion>{
						regions},
			});
		}
	}
}

void prefetch_never_faults(engine_tests::EngineHarness &harness)
{
	const auto protected_base =
		literal_test::code_base + 11U * literal_test::page_size;
	std::vector<std::byte> memory(literal_test::page_size);
	literal_test::fill_memory(memory, 0xd1U);
	const std::array regions{
		engine_tests::MemoryRegion{
			.address = protected_base,
			.initial = std::span<const std::byte>{memory},
			.readable = false,
			.writable = false,
		},
	};
	for (unsigned operation = 0; operation < 32; ++operation) {
		auto initial = literal_test::seeded_state(
			0x5000'0000'0000'0000ULL | operation);
		char protected_name[88];
		std::snprintf(protected_name, sizeof(protected_name),
			"prfm-literal protected operation=%u", operation);
		harness.check({
			.name = protected_name,
			.instruction = literal_test::literal(
				false, 3,
				literal_test::immediate_for_target(
					literal_test::code_base, protected_base),
				operation),
			.initial = initial,
			.expected = initial,
			.native = engine_tests::NativeRequirements::baseline(),
			.memory =
				std::span<const engine_tests::MemoryRegion>{regions},
		});

		const auto unmapped =
			literal_test::code_base - 11U * literal_test::page_size +
			operation * 4U;
		char unmapped_name[88];
		std::snprintf(unmapped_name, sizeof(unmapped_name),
			"prfm-literal unmapped operation=%u", operation);
		harness.check_one(unmapped_name,
			literal_test::literal(
				false, 3,
				literal_test::immediate_for_target(
					literal_test::code_base, unmapped),
				operation),
			initial, initial,
			engine_tests::NativeRequirements::baseline());
	}
}

} // namespace

int main()
{
	engine_tests::EngineHarness harness;
	unmapped_and_protected_faults(harness);
	cross_page_success(harness);
	cross_page_fault_atomicity(harness);
	prefetch_never_faults(harness);
	harness.print_summary("arm64-literal-load-fault");
	return 0;
}
