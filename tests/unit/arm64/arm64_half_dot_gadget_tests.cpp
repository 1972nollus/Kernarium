// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_half_dot_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "platform/apple/host_features.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

using uml::testing::a64::conditional_branch;
using uml::testing::a64::indirect_branch;

extern "C" {
void uml_arm64_gadget_half_dot_native_vector_2s();
void uml_arm64_gadget_half_dot_native_vector_4s();
void uml_arm64_gadget_half_dot_native_indexed_2s_0();
void uml_arm64_gadget_half_dot_native_indexed_2s_1();
void uml_arm64_gadget_half_dot_native_indexed_2s_2();
void uml_arm64_gadget_half_dot_native_indexed_2s_3();
void uml_arm64_gadget_half_dot_native_indexed_4s_0();
void uml_arm64_gadget_half_dot_native_indexed_4s_1();
void uml_arm64_gadget_half_dot_native_indexed_4s_2();
void uml_arm64_gadget_half_dot_native_indexed_4s_3();
void uml_arm64_gadget_half_dot_software();
}

namespace test = arm64_half_dot_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using Gadget = void (*)();

constexpr std::array<Gadget, 10> native_gadgets{
	uml_arm64_gadget_half_dot_native_vector_2s,
	uml_arm64_gadget_half_dot_native_vector_4s,
	uml_arm64_gadget_half_dot_native_indexed_2s_0,
	uml_arm64_gadget_half_dot_native_indexed_2s_1,
	uml_arm64_gadget_half_dot_native_indexed_2s_2,
	uml_arm64_gadget_half_dot_native_indexed_2s_3,
	uml_arm64_gadget_half_dot_native_indexed_4s_0,
	uml_arm64_gadget_half_dot_native_indexed_4s_1,
	uml_arm64_gadget_half_dot_native_indexed_4s_2,
	uml_arm64_gadget_half_dot_native_indexed_4s_3,
};

[[nodiscard]] std::uintptr_t address(Gadget gadget)
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] constexpr std::uintptr_t vector_offset(unsigned reg)
{
	return offsetof(test::arm64::CpuState, vectors) +
		reg * sizeof(test::arm64::VectorRegister);
}

[[nodiscard]] constexpr std::uintptr_t translated_operand(
	test::Form form, test::Registers registers)
{
	const unsigned descriptor =
		static_cast<unsigned>(form.by_element) |
		(static_cast<unsigned>(form.wide) << 1U) |
		(form.index << 2U);
	static_assert(vector_offset(31U) <= 0xffffU);
	return vector_offset(registers.destination) |
		(vector_offset(registers.first) << 16U) |
		(vector_offset(registers.second) << 32U) |
		(static_cast<std::uintptr_t>(descriptor) << 48U);
}

void require_native_straight_line(Gadget gadget)
{
	const auto *instructions =
		reinterpret_cast<const std::uint32_t *>(gadget);
	/*
	 * The common three-instruction lazy FP-environment prefix is shared by
	 * every FP gadget. Once active, this family's translated body is entirely
	 * straight-line through indirect dispatch.
	 */
	for (unsigned index = 3U; index < 48U; ++index) {
		test::require(!conditional_branch(instructions[index]),
			"FDOT native gadget contains a conditional branch");
		test::require(
			(instructions[index] & 0xfc00'0000U) != 0x9400'0000U,
			"FDOT native gadget contains a helper call");
		if (indirect_branch(instructions[index]))
			return;
	}
	test::fail("FDOT native gadget did not reach indirect dispatch");
}

void translation_selects_shape_host_path_and_direct_offsets()
{
	constexpr auto base =
		test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31U, 30U, 29U};
	const bool native =
		test::engine_tests::host_supports(test::native_requirements());
	std::set<std::uintptr_t> selected;
	for (unsigned index = 0U; index < test::forms.size(); ++index) {
		const auto form = test::forms[index];
		const std::array code{
			test::instruction(form, registers),
			std::uint32_t{0xd400'0001U},
		};
		const auto built = gadgets::Builder{}.build(base, code);
		test::require(built.complete,
			"FDOT valid block did not build");
		test::require(
			built.program.instruction_count() == 2U,
			"FDOT changed block instruction count");
		test::require(built.program.accesses_fp_state(),
			"FDOT lost FP-state tracking");
		const auto words = built.program.words();
		test::require(words.size() >= 2U,
			"FDOT gadget stream is truncated");
		const auto expected = native ? native_gadgets[index] :
			uml_arm64_gadget_half_dot_software;
		test::require(words[0] == address(expected),
			"FDOT host path or shape remained runtime-decoded");
		test::require(words[1] ==
			translated_operand(form, registers),
			"FDOT registers were not translated to direct offsets");
		selected.insert(words[0]);
	}
	test::require(selected.size() == (native ? 10U : 1U),
		"FDOT selected translation has a dead duplicate");
}

void every_declared_entry_is_distinct_and_native_is_branchless()
{
	std::set<std::uintptr_t> declared{
		address(uml_arm64_gadget_half_dot_software),
	};
	for (const auto gadget : native_gadgets) {
		declared.insert(address(gadget));
		require_native_straight_line(gadget);
	}
	test::require(declared.size() == 11U,
		"FDOT gadget symbol inventory is incomplete or duplicated");
}

} // namespace

int main()
{
	translation_selects_shape_host_path_and_direct_offsets();
	every_declared_entry_is_distinct_and_native_is_branchless();
	return 0;
}
