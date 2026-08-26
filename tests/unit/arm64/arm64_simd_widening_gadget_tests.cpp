// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_widening_test_support.hpp"

#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

extern "C" {
#define UML_DECLARE_WIDENING(name) \
	void uml_arm64_gadget_simd_widening_##name##_8b(); \
	void uml_arm64_gadget_simd_widening_##name##_4h(); \
	void uml_arm64_gadget_simd_widening_##name##_2s(); \
	void uml_arm64_gadget_simd_widening_##name##_16b(); \
	void uml_arm64_gadget_simd_widening_##name##_8h(); \
	void uml_arm64_gadget_simd_widening_##name##_4s();
UML_DECLARE_WIDENING(saddl)
UML_DECLARE_WIDENING(uaddl)
UML_DECLARE_WIDENING(saddw)
UML_DECLARE_WIDENING(uaddw)
UML_DECLARE_WIDENING(ssubl)
UML_DECLARE_WIDENING(usubl)
UML_DECLARE_WIDENING(ssubw)
UML_DECLARE_WIDENING(usubw)
UML_DECLARE_WIDENING(sabal)
UML_DECLARE_WIDENING(uabal)
UML_DECLARE_WIDENING(sabdl)
UML_DECLARE_WIDENING(uabdl)
#undef UML_DECLARE_WIDENING
}

namespace test = arm64_simd_widening_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using Gadget = void (*)();
using GadgetSet = std::array<Gadget, 6>;

#define UML_WIDENING_SET(name) GadgetSet{ \
	uml_arm64_gadget_simd_widening_##name##_8b, \
	uml_arm64_gadget_simd_widening_##name##_4h, \
	uml_arm64_gadget_simd_widening_##name##_2s, \
	uml_arm64_gadget_simd_widening_##name##_16b, \
	uml_arm64_gadget_simd_widening_##name##_8h, \
	uml_arm64_gadget_simd_widening_##name##_4s}

constexpr std::array expected_gadgets{
	std::array{UML_WIDENING_SET(saddl), UML_WIDENING_SET(uaddl)},
	std::array{UML_WIDENING_SET(saddw), UML_WIDENING_SET(uaddw)},
	std::array{UML_WIDENING_SET(ssubl), UML_WIDENING_SET(usubl)},
	std::array{UML_WIDENING_SET(ssubw), UML_WIDENING_SET(usubw)},
	std::array{UML_WIDENING_SET(sabal), UML_WIDENING_SET(uabal)},
	std::array{UML_WIDENING_SET(sabdl), UML_WIDENING_SET(uabdl)},
};

#undef UML_WIDENING_SET

[[nodiscard]] std::uintptr_t address_of(Gadget gadget)
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] constexpr std::uintptr_t vector_offset(unsigned index)
{
	return offsetof(test::arm64::CpuState, vectors) +
		index * sizeof(test::arm64::VectorRegister);
}

[[nodiscard]] constexpr bool conditional_branch(std::uint32_t instruction)
{
	return (instruction & 0xff00'0010U) == 0x5400'0000U ||
		(instruction & 0x7e00'0000U) == 0x3400'0000U ||
		(instruction & 0x7e00'0000U) == 0x3600'0000U;
}

[[nodiscard]] constexpr bool indirect_branch(std::uint32_t instruction)
{
	return (instruction & 0xffff'fc1fU) == 0xd61f'0000U;
}

[[nodiscard]] unsigned instructions_to_dispatch(Gadget gadget)
{
	const auto *instructions =
		reinterpret_cast<const std::uint32_t *>(gadget);
	for (unsigned index = 0; index < 64U; ++index) {
		test::require(!conditional_branch(instructions[index]),
			"widening gadget contains a conditional branch");
		if (indirect_branch(instructions[index]))
			return index + 1U;
	}
	test::fail("widening gadget did not reach indirect dispatch");
}

void every_form_uses_one_direct_offset_gadget()
{
	constexpr auto base = test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31U, 30U, 29U};
	std::set<std::uintptr_t> selected;
	std::set<std::uintptr_t> expected;
	for (const auto &operation : expected_gadgets)
		for (const auto &signedness : operation)
			for (const auto gadget : signedness)
				expected.insert(address_of(gadget));

	for (unsigned operation = 0; operation < test::operations.size();
	     ++operation)
		for (unsigned unsigned_values = 0; unsigned_values < 2U;
		     ++unsigned_values)
			for (unsigned shape = 0; shape < test::shapes.size();
			     ++shape) {
				const auto built = gadgets::Builder{}.build(base,
					std::array{test::instruction(
						test::operations[operation],
						unsigned_values != 0U,
						test::shapes[shape], registers)});
				test::require(built.complete,
					"allocated widening instruction did not build");
				const auto words = built.program.words();
				test::require(words.size() == 4U,
					"widening gadget stream has an unexpected shape");
				const auto selected_gadget =
					expected_gadgets[operation][unsigned_values][shape];
				test::require(words[0] == address_of(selected_gadget),
					"operation or arrangement was not selected in translation");
				test::require(words[1] ==
						gadgets::selection::packed_u16_operands(
							vector_offset(registers.destination),
							vector_offset(registers.left),
							vector_offset(registers.right)),
					"register offsets were not packed during translation");
				selected.insert(words[0]);
			}
	test::require(expected.size() == 72U,
		"expected widening gadget table is incomplete");
	test::require(selected == expected,
		"widening gadget table contains dead or unselected entries");
}

void reserved_forms_never_select_a_widening_gadget()
{
	constexpr auto base = test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31U, 30U, 29U};
	std::set<std::uintptr_t> expected;
	for (const auto &operation : expected_gadgets)
		for (const auto &signedness : operation)
			for (const auto gadget : signedness)
				expected.insert(address_of(gadget));
	for (const auto operation : test::operations)
		for (const bool unsigned_values : {false, true}) {
			for (const bool upper : {false, true}) {
				const auto built = gadgets::Builder{}.build(base,
					std::array{test::instruction(operation,
						unsigned_values, {3U, upper}, registers)});
				test::require(built.complete,
					"reserved widening size did not terminate");
				test::require(expected.count(
					built.program.words()[0]) == 0U,
					"reserved widening size selected a valid gadget");
			}
			for (unsigned size = 0; size < 4U; ++size) {
				const auto built = gadgets::Builder{}.build(base,
					std::array{test::scalar_instruction(operation,
						unsigned_values, size, registers)});
				test::require(built.complete,
					"reserved scalar widening form did not terminate");
				test::require(expected.count(
					built.program.words()[0]) == 0U,
					"reserved scalar form selected a valid gadget");
			}
		}
}

void every_hot_path_is_minimal_and_branch_free()
{
	for (unsigned operation = 0; operation < expected_gadgets.size();
	     ++operation)
		for (const auto &signedness : expected_gadgets[operation])
			for (const auto gadget : signedness) {
				const unsigned expected_count =
					test::operations[operation] ==
						test::Operation::absolute_accumulate ?
					11U : 10U;
				test::require(instructions_to_dispatch(gadget) ==
						expected_count,
					"widening gadget has unexpected runtime work");
			}
}

} // namespace

int main()
{
	every_form_uses_one_direct_offset_gadget();
	reserved_forms_never_select_a_widening_gadget();
	every_hot_path_is_minimal_and_branch_free();
	return 0;
}
