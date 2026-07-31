// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_structure_test_support.hpp"

#include <array>
#include <cstdio>

namespace structure = arm64_simd_structure_test;
namespace engine = uml::backend::tests;

namespace {

void complete_lane_allocation_partition(engine::EngineHarness &harness)
{
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0xa7);
	const auto address = structure::data_base + structure::page_size / 2U;
	std::uint64_t seed = 0xa400'0000'0000'0000ULL;

	for (const bool full_vector : {false, true})
		for (const bool load : {false, true})
			for (unsigned extra = 0; extra < 2U; ++extra)
				for (unsigned opcode = 0; opcode < 6U; ++opcode)
					for (unsigned scale_bit = 0; scale_bit < 2U;
					     ++scale_bit)
						for (unsigned size = 0; size < 4U; ++size)
							for (const bool post_index :
							     {false, true})
								for (unsigned rm = 0; rm < 32U;
								     ++rm) {
								const auto form =
									structure::decode_single(
										full_vector, load, extra,
										opcode, scale_bit, size);
								const bool valid =
									form.valid &&
									(post_index || rm == 0U);
								const auto instruction =
									structure::single_instruction(
										full_vector, load,
										post_index, extra, rm, opcode,
										scale_bit, size, 2, 3);
								if (!valid &&
								    structure::ordered_allocation_overlap(
									    instruction))
									continue;
								char name[168];
								std::snprintf(name, sizeof(name),
									"lane allocation q=%u l=%u r=%u "
									"opcode=%u s=%u size=%u post=%u "
									"rm=%u",
									static_cast<unsigned>(full_vector),
									static_cast<unsigned>(load), extra,
									opcode, scale_bit, size,
									static_cast<unsigned>(post_index),
									rm);
								if (!valid) {
									auto initial =
										structure::seeded_state(
											seed++);
									structure::set_base(
										initial, 2, address);
									harness.check_illegal(
										name, instruction, initial,
										structure::
											native_requirement());
									continue;
								}
								const auto mode = !post_index ?
										structure::AddressMode::
											no_offset :
									rm == 31U ?
										structure::AddressMode::
											post_immediate :
										structure::AddressMode::
											post_register;
								structure::check_single_success(
									harness, name, form, full_vector,
									load, extra, opcode, scale_bit,
									size, mode, 2, 3, rm,
									address, memory, seed++);
							}
}

void decode_precedes_stack_alignment(engine::EngineHarness &harness)
{
	constexpr std::array cases{
		structure::single_instruction(
			true, true, false, 0, 0, 2, 0, 1, 31, 3),
		structure::single_instruction(
			true, false, false, 0, 0, 4, 1, 1, 31, 3),
		structure::single_instruction(
			true, true, false, 0, 2, 0, 0, 0, 31, 3),
	};
	for (unsigned index = 0; index < cases.size(); ++index) {
		auto initial = structure::seeded_state(
			0xa4ff'0000'0000'0000ULL | index);
		initial.sp = structure::data_base + 8U;
		char name[96];
		std::snprintf(name, sizeof(name),
			"lane reserved-before-sp-alignment case=%u", index);
		harness.check_illegal(name, cases[index], initial,
			structure::native_requirement());
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	complete_lane_allocation_partition(harness);
	decode_precedes_stack_alignment(harness);
	harness.print_summary("arm64-simd-structure-lane-allocation");
	return 0;
}
