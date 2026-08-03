// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_ordinary_memory_test_support.hpp"

#include <array>
#include <cstdio>

namespace memory = arm64_simd_ordinary_memory_test;
namespace engine = uml::emulator::tests;

namespace {

constexpr std::array immediate_modes{
	memory::ImmediateMode::unscaled,
	memory::ImmediateMode::post_index,
	memory::ImmediateMode::pre_index,
};

void reserved_width_operation_cells(engine::EngineHarness &harness)
{
	auto initial = memory::seeded_state(0x1000);
	for (unsigned size = 1; size < 4U; ++size)
		for (unsigned operation = 2; operation < 4U; ++operation) {
			for (const unsigned imm12 :
			     {0U, 1U, 0x555U, 0xaaaU, 0xfffU}) {
				char name[128];
				std::snprintf(name, sizeof(name),
					"unsigned reserved size=%u opc=%u imm12=%u",
					size, operation, imm12);
				harness.check_illegal(name,
					memory::unsigned_instruction(
						size, operation, imm12, 2, 29),
					initial, memory::single_native());
			}
			for (const auto mode : immediate_modes)
				for (const unsigned imm9 :
				     {0U, 1U, 0xffU, 0x100U, 0x1ffU}) {
					char name[136];
					std::snprintf(name, sizeof(name),
						"immediate reserved size=%u opc=%u "
						"mode=%u imm9=%u",
						size, operation,
						static_cast<unsigned>(mode), imm9);
					harness.check_illegal(name,
						memory::immediate_instruction(
							size, operation, imm9, mode,
							2, 29),
						initial, memory::single_native());
				}
			for (const unsigned option : {2U, 3U, 6U, 7U})
				for (const bool scaled : {false, true}) {
					char name[136];
					std::snprintf(name, sizeof(name),
						"register reserved size=%u opc=%u "
						"option=%u scaled=%u",
						size, operation, option,
						static_cast<unsigned>(scaled));
					harness.check_illegal(name,
						memory::register_instruction(
							size, operation, 5, option,
							scaled, 2, 29),
						initial, memory::single_native());
				}
		}
}

void reserved_register_extensions(engine::EngineHarness &harness)
{
	auto initial = memory::seeded_state(0x2000);
	for (const auto &variant : memory::single_variants)
		for (const unsigned option : {0U, 1U, 4U, 5U})
			for (const bool scaled : {false, true}) {
				char name[144];
				std::snprintf(name, sizeof(name),
					"%.*s reserved option=%u scaled=%u",
					static_cast<int>(variant.name.size()),
					variant.name.data(), option,
					static_cast<unsigned>(scaled));
				harness.check_illegal(name,
					memory::register_instruction(
						variant.size_code, variant.operation, 5,
						option, scaled, 2, 29),
					initial, memory::single_native());
			}
}

void reserved_immediate_mode(engine::EngineHarness &harness)
{
	auto initial = memory::seeded_state(0x2800);
	/*
	 * With the register-offset selector clear, mode 10 has no SIMD&FP
	 * immediate allocation. The same mode is allocated only when bit 21
	 * selects the register-offset encoding.
	 */
	for (const auto &variant : memory::single_variants)
		for (const unsigned imm9 :
		     {0U, 1U, 0xffU, 0x100U, 0x1ffU}) {
			char name[144];
			std::snprintf(name, sizeof(name),
				"%.*s reserved immediate mode=2 imm9=%u",
				static_cast<int>(variant.name.size()),
				variant.name.data(), imm9);
			harness.check_illegal(name,
				memory::immediate_instruction(
					variant.size_code, variant.operation, imm9,
					static_cast<memory::ImmediateMode>(2U), 2, 29),
				initial, memory::single_native());
		}
}

void duplicate_pair_load_destinations(engine::EngineHarness &harness)
{
	auto initial = memory::seeded_state(0x3000);
	for (unsigned mode = 0; mode < 4U; ++mode)
		for (unsigned destination = 0; destination < 32U; ++destination)
			for (const unsigned imm7 :
			     {0U, 1U, 0x3fU, 0x40U, 0x7fU}) {
				char name[144];
				std::snprintf(name, sizeof(name),
					"pair load duplicate mode=%u destination=%u "
					"imm7=%u",
					mode, destination, imm7);
				harness.check_illegal(name,
					memory::pair_instruction(
						true, static_cast<memory::PairMode>(mode),
						imm7, 2, destination, destination),
					initial, memory::pair_native());
			}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	reserved_width_operation_cells(harness);
	reserved_register_extensions(harness);
	reserved_immediate_mode(harness);
	duplicate_pair_load_destinations(harness);
	harness.print_summary("arm64-simd-ordinary-memory-allocation");
}
