// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_ordinary_memory_test_support.hpp"

#include <array>
#include <cstdio>

namespace memory = arm64_simd_ordinary_memory_test;
namespace engine = uml::emulator::tests;
namespace arm64 = uml::cpu::arm64;

namespace {

void cross_page_success(engine::EngineHarness &harness,
	std::span<const std::byte> pages, std::uint64_t &seed)
{
	const auto boundary = memory::data_base + memory::page_size;
	for (const auto &variant : memory::single_variants) {
		const auto target = boundary -
			(variant.size == 1U ? 0U : variant.size / 2U);
		for (unsigned form = 0; form < 5U; ++form) {
			auto initial = memory::seeded_state(seed++);
			constexpr unsigned rn = 20;
			constexpr unsigned rt = 29;
			std::uint32_t instruction{};
			std::uint64_t base{};
			std::uint64_t adjusted{};
			bool writeback = false;
			switch (form) {
			case 0:
				base = target - variant.size;
				instruction = memory::unsigned_instruction(
					variant.size_code, variant.operation,
					1, rn, rt);
				break;
			case 1:
				base = target - 3U;
				instruction = memory::immediate_instruction(
					variant.size_code, variant.operation, 3,
					memory::ImmediateMode::unscaled, rn, rt);
				break;
			case 2:
				base = target;
				adjusted = base + 16U;
				writeback = true;
				instruction = memory::immediate_instruction(
					variant.size_code, variant.operation, 16,
					memory::ImmediateMode::post_index, rn, rt);
				break;
			case 3:
				base = target - 16U;
				adjusted = target;
				writeback = true;
				instruction = memory::immediate_instruction(
					variant.size_code, variant.operation, 16,
					memory::ImmediateMode::pre_index, rn, rt);
				break;
			default:
				base = target - 8U;
				initial.registers[5] = 8U;
				instruction = memory::register_instruction(
					variant.size_code, variant.operation,
					5, 3, false, rn, rt);
				break;
			}
			initial.registers[rn] = base;
			auto expected = initial;
			auto expected_memory =
				std::vector<std::byte>(pages.begin(), pages.end());
			memory::apply_single_oracle(initial, expected, pages,
				expected_memory,
				static_cast<std::size_t>(
					target - memory::data_base),
				variant, rt);
			if (writeback)
				expected.registers[rn] = adjusted;
			const std::array regions{
				engine::MemoryRegion{
					.address = memory::data_base,
					.initial = pages,
					.expected =
						std::span<const std::byte>{
							expected_memory},
				},
			};
			char name[152];
			std::snprintf(name, sizeof(name),
				"%.*s mapped page boundary form=%u",
				static_cast<int>(variant.name.size()),
				variant.name.data(), form);
			memory::check(harness, name, instruction, initial, expected,
				memory::single_native(), regions);
		}
	}

	const auto target = boundary - 16U;
	for (unsigned mode = 0; mode < 4U; ++mode)
		for (const bool load : {false, true}) {
			const auto pair_mode =
				static_cast<memory::PairMode>(mode);
			constexpr unsigned imm7 = 1U;
			const auto base =
				pair_mode == memory::PairMode::post_index ?
					target : target - 16U;
			auto initial = memory::seeded_state(seed++);
			initial.registers[20] = base;
			auto expected = initial;
			auto expected_memory =
				std::vector<std::byte>(pages.begin(), pages.end());
			memory::apply_pair_oracle(initial, expected, pages,
				expected_memory,
				static_cast<std::size_t>(
					target - memory::data_base),
				load, 3, 17);
			if (pair_mode == memory::PairMode::post_index ||
			    pair_mode == memory::PairMode::pre_index)
				expected.registers[20] = base + 16U;
			const std::array regions{
				engine::MemoryRegion{
					.address = memory::data_base,
					.initial = pages,
					.expected =
						std::span<const std::byte>{
							expected_memory},
				},
			};
			char name[144];
			std::snprintf(name, sizeof(name),
				"%s mapped page boundary mode=%u",
				load ? "load-pair" : "store-pair", mode);
			memory::check(harness, name,
				memory::pair_instruction(
					load, pair_mode, imm7, 20, 3, 17),
				initial, expected, memory::pair_native(), regions);
		}
}

void stack_alignment_priority(engine::EngineHarness &harness)
{
	for (const auto &variant : memory::single_variants) {
		auto initial = memory::seeded_state(0xc000 + variant.size);
		initial.sp =
			memory::data_base + memory::page_size / 2U + 8U;
		for (const auto mode : {
		     memory::ImmediateMode::unscaled,
		     memory::ImmediateMode::post_index,
		     memory::ImmediateMode::pre_index}) {
			char name[144];
			std::snprintf(name, sizeof(name),
				"%.*s misaligned-sp immediate mode=%u",
				static_cast<int>(variant.name.size()),
				variant.name.data(), static_cast<unsigned>(mode));
			memory::check(harness, name,
				memory::immediate_instruction(
					variant.size_code, variant.operation,
					0x100U, mode, 31, 29),
				initial, initial, memory::single_native(), {},
				engine::ExpectedEvent::memory_fault,
				{
					.address = initial.sp,
					.access = variant.load ?
						arm64::MemoryAccess::read :
						arm64::MemoryAccess::write,
				},
				true);
		}
		char unsigned_name[144];
		std::snprintf(unsigned_name, sizeof(unsigned_name),
			"%.*s misaligned-sp unsigned",
			static_cast<int>(variant.name.size()), variant.name.data());
		memory::check(harness, unsigned_name,
			memory::unsigned_instruction(
				variant.size_code, variant.operation,
				0xfffU, 31, 29),
			initial, initial, memory::single_native(), {},
			engine::ExpectedEvent::memory_fault,
			{
				.address = initial.sp,
				.access = variant.load ?
					arm64::MemoryAccess::read :
					arm64::MemoryAccess::write,
			},
			true);

		char register_name[144];
		std::snprintf(register_name, sizeof(register_name),
			"%.*s misaligned-sp register",
			static_cast<int>(variant.name.size()), variant.name.data());
		memory::check(harness, register_name,
			memory::register_instruction(
				variant.size_code, variant.operation,
				5, 7, true, 31, 29),
			initial, initial, memory::single_native(), {},
			engine::ExpectedEvent::memory_fault,
			{
				.address = initial.sp,
				.access = variant.load ?
					arm64::MemoryAccess::read :
					arm64::MemoryAccess::write,
			},
			true);
	}

	for (unsigned mode = 0; mode < 4U; ++mode)
		for (const bool load : {false, true}) {
			auto initial =
				memory::seeded_state(0xd000 + mode * 2U + load);
			initial.sp =
				memory::data_base + memory::page_size / 2U + 8U;
			char name[128];
			std::snprintf(name, sizeof(name),
				"%s misaligned-sp mode=%u",
				load ? "load-pair" : "store-pair", mode);
			memory::check(harness, name,
				memory::pair_instruction(
					load, static_cast<memory::PairMode>(mode),
					0x40U, 31, 3, 17),
				initial, initial, memory::pair_native(), {},
				engine::ExpectedEvent::memory_fault,
				{
					.address = initial.sp,
					.access = load ?
						arm64::MemoryAccess::read :
						arm64::MemoryAccess::write,
				},
				true);
		}
}

void permission_faults(engine::EngineHarness &harness,
	std::span<const std::byte> page, std::uint64_t &seed)
{
	const auto target = memory::data_base + memory::page_size / 2U;
	for (const auto &variant : memory::single_variants) {
		auto initial = memory::seeded_state(seed++);
		initial.registers[20] = target;
		const std::array regions{
			engine::MemoryRegion{
				.address = memory::data_base,
				.initial = page,
				.readable = !variant.load,
				.writable = variant.load,
			},
		};
		char name[136];
		std::snprintf(name, sizeof(name),
			"%.*s permission fault",
			static_cast<int>(variant.name.size()), variant.name.data());
		memory::check(harness, name,
			memory::immediate_instruction(
				variant.size_code, variant.operation, 7,
				memory::ImmediateMode::post_index, 20, 29),
			initial, initial, memory::single_native(), regions,
			engine::ExpectedEvent::memory_fault,
			{
				.address = target,
				.access = variant.load ?
					arm64::MemoryAccess::read :
					arm64::MemoryAccess::write,
			},
			true);
	}

	for (unsigned mode = 0; mode < 4U; ++mode)
		for (const bool load : {false, true}) {
			auto initial = memory::seeded_state(seed++);
			const unsigned imm7 = mode == 1U ? 7U : 0U;
			initial.registers[20] = target;
			const std::array regions{
				engine::MemoryRegion{
					.address = memory::data_base,
					.initial = page,
					.readable = !load,
					.writable = load,
				},
			};
			char name[128];
			std::snprintf(name, sizeof(name),
				"%s permission fault mode=%u",
				load ? "load-pair" : "store-pair", mode);
			memory::check(harness, name,
				memory::pair_instruction(
					load, static_cast<memory::PairMode>(mode),
					imm7, 20, 3, 17),
				initial, initial, memory::pair_native(), regions,
				engine::ExpectedEvent::memory_fault,
				{
					.address = target,
					.access = load ?
						arm64::MemoryAccess::read :
						arm64::MemoryAccess::write,
				},
				true);
		}
}

void cross_page_fault_atomicity(engine::EngineHarness &harness,
	std::span<const std::byte> page, std::uint64_t &seed)
{
	const auto boundary = memory::data_base + memory::page_size;
	for (const auto &variant : memory::single_variants) {
		const auto target = boundary -
			(variant.size == 1U ? 0U : variant.size / 2U);
		auto initial = memory::seeded_state(seed++);
		initial.registers[20] = target;
		const std::array regions{
			engine::MemoryRegion{
				.address = memory::data_base,
				.initial = page,
			},
		};
		char name[144];
		std::snprintf(name, sizeof(name),
			"%.*s cross-page fault atomicity",
			static_cast<int>(variant.name.size()), variant.name.data());
		memory::check(harness, name,
			memory::register_instruction(
				variant.size_code, variant.operation,
				31, 3, false, 20, 29),
			initial, initial, memory::single_native(), regions,
			engine::ExpectedEvent::memory_fault,
			{
				.address = boundary,
				.access = variant.load ?
					arm64::MemoryAccess::read :
					arm64::MemoryAccess::write,
			},
			true);
	}

	const auto target = boundary - 16U;
	for (unsigned mode = 0; mode < 4U; ++mode)
		for (const bool load : {false, true}) {
			auto initial = memory::seeded_state(seed++);
			initial.registers[20] = target;
			const std::array regions{
				engine::MemoryRegion{
					.address = memory::data_base,
					.initial = page,
				},
			};
			char name[144];
			std::snprintf(name, sizeof(name),
				"%s cross-page atomicity mode=%u",
				load ? "load-pair" : "store-pair", mode);
			memory::check(harness, name,
				memory::pair_instruction(
					load, static_cast<memory::PairMode>(mode),
					0, 20, 3, 17),
				initial, initial, memory::pair_native(), regions,
				engine::ExpectedEvent::memory_fault,
				{
					.address = boundary,
					.access = load ?
						arm64::MemoryAccess::read :
						arm64::MemoryAccess::write,
				},
				true);
		}
}

void reserved_precedes_alignment(engine::EngineHarness &harness)
{
	auto initial = memory::seeded_state(0xe000);
	initial.sp = memory::data_base + 8U;
	for (unsigned size = 1; size < 4U; ++size)
		for (unsigned operation = 2; operation < 4U; ++operation) {
			char name[128];
			std::snprintf(name, sizeof(name),
				"reserved-before-alignment size=%u opc=%u",
				size, operation);
			harness.check_illegal(name,
				memory::unsigned_instruction(
					size, operation, 0xfffU, 31, 29),
				initial, memory::single_native());
		}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	std::vector<std::byte> page(memory::page_size);
	memory::fill_memory(page, 0xd3);
	std::vector<std::byte> pages(2U * memory::page_size);
	memory::fill_memory(pages, 0xc1);
	std::uint64_t seed = 0xf000;
	cross_page_success(harness, pages, seed);
	stack_alignment_priority(harness);
	permission_faults(harness, page, seed);
	cross_page_fault_atomicity(harness, page, seed);
	reserved_precedes_alignment(harness);
	harness.print_summary("arm64-simd-ordinary-memory-fault");
}
