// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_high_narrow_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

extern "C" {
#define UML_DECLARE_HIGH_NARROW(name) \
	void uml_arm64_gadget_simd_widening_##name##_8h(); \
	void uml_arm64_gadget_simd_widening_##name##_4s(); \
	void uml_arm64_gadget_simd_widening_##name##_2d(); \
	void uml_arm64_gadget_simd_widening_##name##_8h2(); \
	void uml_arm64_gadget_simd_widening_##name##_4s2(); \
	void uml_arm64_gadget_simd_widening_##name##_2d2();
UML_DECLARE_HIGH_NARROW(addhn)
UML_DECLARE_HIGH_NARROW(raddhn)
UML_DECLARE_HIGH_NARROW(subhn)
UML_DECLARE_HIGH_NARROW(rsubhn)
#undef UML_DECLARE_HIGH_NARROW
}

namespace test = arm64_simd_high_narrow_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using Gadget = void (*)();

#define UML_HIGH_NARROW_SET(name) std::array<Gadget, 6>{ \
	uml_arm64_gadget_simd_widening_##name##_8h, \
	uml_arm64_gadget_simd_widening_##name##_4s, \
	uml_arm64_gadget_simd_widening_##name##_2d, \
	uml_arm64_gadget_simd_widening_##name##_8h2, \
	uml_arm64_gadget_simd_widening_##name##_4s2, \
	uml_arm64_gadget_simd_widening_##name##_2d2}

constexpr std::array expected_gadgets{
	UML_HIGH_NARROW_SET(addhn),
	UML_HIGH_NARROW_SET(raddhn),
	UML_HIGH_NARROW_SET(subhn),
	UML_HIGH_NARROW_SET(rsubhn),
};

#undef UML_HIGH_NARROW_SET

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

void require_branch_free(Gadget gadget)
{
	const auto *instructions =
		reinterpret_cast<const std::uint32_t *>(gadget);
	bool reached_dispatch = false;
	for (unsigned index = 0; index < 64U; ++index) {
		test::require(!conditional_branch(instructions[index]),
			"high-narrow gadget contains a conditional branch");
		if (indirect_branch(instructions[index])) {
			reached_dispatch = true;
			break;
		}
	}
	test::require(reached_dispatch,
		"high-narrow gadget did not reach indirect dispatch");
}

void every_form_uses_one_direct_offset_gadget()
{
	static_assert(vector_offset(31U) <= 0xffffU);
	constexpr auto base = test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31U, 30U, 29U};
	std::set<std::uintptr_t> selected;
	std::set<std::uintptr_t> expected;
	for (const auto &operation : expected_gadgets)
		for (const auto gadget : operation)
			expected.insert(address_of(gadget));

	for (unsigned operation = 0; operation < test::operations.size();
	     ++operation)
		for (unsigned shape = 0; shape < test::shapes.size(); ++shape) {
			const auto built = gadgets::Builder{}.build(base,
				std::array{test::instruction(
					test::operations[operation], test::shapes[shape],
					registers)});
			test::require(built.complete,
				"allocated high-narrow instruction did not build");
			const auto words = built.program.words();
			if (words.size() != 6U)
				std::fprintf(stderr,
					"high-narrow stream words=%zu operation=%u shape=%u\n",
					words.size(), operation, shape);
			test::require(words.size() == 6U,
				"high-narrow gadget stream has an unexpected shape");
			test::require(words[0] ==
					address_of(expected_gadgets[operation][shape]),
				"operation or arrangement was not selected in translation");
			test::require(words[1] ==
					vector_offset(registers.destination) &&
					words[2] == vector_offset(registers.left) &&
					words[3] == vector_offset(registers.right),
				"register fields were not translated to direct offsets");
			selected.insert(words[0]);
		}
	test::require(expected.size() == 24U,
		"expected high-narrow gadget table is incomplete");
	test::require(selected == expected,
		"high-narrow gadget table contains dead or unselected entries");
}

void every_hot_path_is_branch_free()
{
	for (const auto &operation : expected_gadgets)
		for (const auto gadget : operation)
			require_branch_free(gadget);
}

} // namespace

int main()
{
	every_form_uses_one_direct_offset_gadget();
	every_hot_path_is_branch_free();
	return 0;
}
