// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_structure_test_support.hpp"

#include <array>
#include <cstdio>

namespace structure = arm64_simd_structure_test;
namespace engine = uml::backend::tests;

namespace {

[[nodiscard]] const structure::MultipleForm *find_form(unsigned opcode)
{
	for (const auto &form : structure::multiple_forms)
		if (form.opcode == opcode)
			return &form;
	return nullptr;
}

void complete_allocation_partition(engine::EngineHarness &harness)
{
	std::vector<std::byte> memory(structure::page_size);
	structure::fill_memory(memory, 0x41);
	const auto address = structure::data_base + structure::page_size / 2U;
	std::uint64_t seed = 0xa100'0000'0000'0000ULL;

	for (unsigned opcode = 0; opcode < 16U; ++opcode)
		for (const bool full_vector : {false, true})
			for (const bool load : {false, true})
				for (unsigned size = 0; size < 4U; ++size)
					for (const bool post_index : {false, true})
						for (unsigned rm = 0; rm < 32U; ++rm) {
						const auto *form = find_form(opcode);
						const bool valid =
							form != nullptr &&
							structure::multiple_arrangement_valid(
								*form, full_vector, size) &&
							(post_index || rm == 0U);
						char name[144];
						std::snprintf(name, sizeof(name),
							"multiple allocation opcode=%u q=%u l=%u "
							"size=%u post=%u rm=%u",
							opcode,
							static_cast<unsigned>(full_vector),
							static_cast<unsigned>(load), size,
							static_cast<unsigned>(post_index), rm);
						if (!valid) {
							auto initial =
								structure::seeded_state(seed++);
							structure::set_base(initial, 2, address);
							harness.check_illegal(name,
								structure::multiple_instruction(
									full_vector, load,
									post_index, rm, opcode, size,
									2, 3),
								initial,
								structure::native_requirement());
							continue;
						}
						const auto mode = !post_index ?
							structure::AddressMode::no_offset :
							rm == 31U ?
								structure::AddressMode::post_immediate :
								structure::AddressMode::post_register;
						structure::check_multiple_success(
							harness, name, *form, full_vector, size,
							load, mode, 2, 3, rm, address,
							memory, seed++);
					}
}

void decode_precedes_stack_alignment(engine::EngineHarness &harness)
{
	constexpr std::array cases{
		structure::multiple_instruction(
			true, true, false, 0, 1, 0, 31, 3),
		structure::multiple_instruction(
			false, false, false, 0, 8, 3, 31, 3),
		structure::multiple_instruction(
			true, true, false, 1, 7, 0, 31, 3),
	};
	for (unsigned index = 0; index < cases.size(); ++index) {
		auto initial = structure::seeded_state(
			0xa1ff'0000'0000'0000ULL | index);
		initial.sp = structure::data_base + 8U;
		char name[96];
		std::snprintf(name, sizeof(name),
			"multiple reserved-before-sp-alignment case=%u", index);
		harness.check_illegal(name, cases[index], initial,
			structure::native_requirement());
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	complete_allocation_partition(harness);
	decode_precedes_stack_alignment(harness);
	harness.print_summary("arm64-simd-structure-multiple-allocation");
	return 0;
}
