// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_simd_dot_test_support.hpp"

#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

using uml::testing::a64::conditional_branch;
using uml::testing::a64::indirect_branch;

extern "C" {
#define UML_DECLARE_DOT_VECTOR(name, shape) \
	void uml_arm64_gadget_simd_dot_vector_##name##_##shape(); \
	void uml_arm64_gadget_simd_dot_vector_##name##_##shape##_soft();
#define UML_DECLARE_DOT_VECTOR_FAMILY(name) \
	UML_DECLARE_DOT_VECTOR(name, 2s) \
	UML_DECLARE_DOT_VECTOR(name, 4s)
UML_DECLARE_DOT_VECTOR_FAMILY(sdot)
UML_DECLARE_DOT_VECTOR_FAMILY(udot)
UML_DECLARE_DOT_VECTOR_FAMILY(usdot)
#undef UML_DECLARE_DOT_VECTOR_FAMILY
#undef UML_DECLARE_DOT_VECTOR

#define UML_DECLARE_DOT_INDEXED_ONE(name, shape, index) \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_##index(); \
	void uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_##index##_soft();
#define UML_DECLARE_DOT_INDEXED_SHAPE(name, shape) \
	UML_DECLARE_DOT_INDEXED_ONE(name, shape, 0) \
	UML_DECLARE_DOT_INDEXED_ONE(name, shape, 1) \
	UML_DECLARE_DOT_INDEXED_ONE(name, shape, 2) \
	UML_DECLARE_DOT_INDEXED_ONE(name, shape, 3)
#define UML_DECLARE_DOT_INDEXED_FAMILY(name) \
	UML_DECLARE_DOT_INDEXED_SHAPE(name, 2s) \
	UML_DECLARE_DOT_INDEXED_SHAPE(name, 4s)
UML_DECLARE_DOT_INDEXED_FAMILY(sdot)
UML_DECLARE_DOT_INDEXED_FAMILY(udot)
UML_DECLARE_DOT_INDEXED_FAMILY(sudot)
UML_DECLARE_DOT_INDEXED_FAMILY(usdot)
#undef UML_DECLARE_DOT_INDEXED_FAMILY
#undef UML_DECLARE_DOT_INDEXED_SHAPE
#undef UML_DECLARE_DOT_INDEXED_ONE
}

namespace test = arm64_simd_dot_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using Gadget = void (*)();

[[nodiscard]] std::uintptr_t address(Gadget gadget) noexcept
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] constexpr std::uintptr_t vector_offset(unsigned reg) noexcept
{
	return offsetof(test::arm64::CpuState, vectors) +
		reg * sizeof(test::arm64::VectorRegister);
}

#define UML_DOT_VECTOR_PAIR(name) std::array<Gadget, 2>{ \
	uml_arm64_gadget_simd_dot_vector_##name##_2s, \
	uml_arm64_gadget_simd_dot_vector_##name##_4s}
#define UML_DOT_VECTOR_SOFT_PAIR(name) std::array<Gadget, 2>{ \
	uml_arm64_gadget_simd_dot_vector_##name##_2s_soft, \
	uml_arm64_gadget_simd_dot_vector_##name##_4s_soft}

[[nodiscard]] Gadget vector_gadget(test::Mode mode, bool q)
{
	static constexpr auto signed_native = UML_DOT_VECTOR_PAIR(sdot);
	static constexpr auto signed_software = UML_DOT_VECTOR_SOFT_PAIR(sdot);
	static constexpr auto unsigned_native = UML_DOT_VECTOR_PAIR(udot);
	static constexpr auto unsigned_software = UML_DOT_VECTOR_SOFT_PAIR(udot);
	static constexpr auto mixed_native = UML_DOT_VECTOR_PAIR(usdot);
	static constexpr auto mixed_software = UML_DOT_VECTOR_SOFT_PAIR(usdot);
	const bool native =
		test::engine_tests::host_supports(test::native_requirements(mode));
	switch (mode) {
	case test::Mode::signed_signed:
		return (native ? signed_native : signed_software)[q];
	case test::Mode::unsigned_unsigned:
		return (native ? unsigned_native : unsigned_software)[q];
	case test::Mode::unsigned_signed:
		return (native ? mixed_native : mixed_software)[q];
	case test::Mode::signed_unsigned:
		break;
	}
	test::fail("unallocated vector signedness");
}

#undef UML_DOT_VECTOR_SOFT_PAIR
#undef UML_DOT_VECTOR_PAIR

#define UML_DOT_INDEXED_SHAPE(name, shape) std::array<Gadget, 4>{ \
	uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_0, \
	uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_1, \
	uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_2, \
	uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_3}
#define UML_DOT_INDEXED_SOFT_SHAPE(name, shape) std::array<Gadget, 4>{ \
	uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_0_soft, \
	uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_1_soft, \
	uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_2_soft, \
	uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_3_soft}
#define UML_DOT_INDEXED_FAMILY(name) std::array<std::array<Gadget, 4>, 2>{ \
	UML_DOT_INDEXED_SHAPE(name, 2s), \
	UML_DOT_INDEXED_SHAPE(name, 4s)}
#define UML_DOT_INDEXED_SOFT_FAMILY(name) \
	std::array<std::array<Gadget, 4>, 2>{ \
		UML_DOT_INDEXED_SOFT_SHAPE(name, 2s), \
		UML_DOT_INDEXED_SOFT_SHAPE(name, 4s)}

[[nodiscard]] Gadget indexed_gadget(
	test::Mode mode, bool q, unsigned index)
{
	static constexpr auto signed_native = UML_DOT_INDEXED_FAMILY(sdot);
	static constexpr auto signed_software =
		UML_DOT_INDEXED_SOFT_FAMILY(sdot);
	static constexpr auto unsigned_native = UML_DOT_INDEXED_FAMILY(udot);
	static constexpr auto unsigned_software =
		UML_DOT_INDEXED_SOFT_FAMILY(udot);
	static constexpr auto signed_mixed_native =
		UML_DOT_INDEXED_FAMILY(sudot);
	static constexpr auto signed_mixed_software =
		UML_DOT_INDEXED_SOFT_FAMILY(sudot);
	static constexpr auto unsigned_mixed_native =
		UML_DOT_INDEXED_FAMILY(usdot);
	static constexpr auto unsigned_mixed_software =
		UML_DOT_INDEXED_SOFT_FAMILY(usdot);
	const bool native =
		test::engine_tests::host_supports(test::native_requirements(mode));
	switch (mode) {
	case test::Mode::signed_signed:
		return (native ? signed_native : signed_software)[q][index];
	case test::Mode::unsigned_unsigned:
		return (native ? unsigned_native : unsigned_software)[q][index];
	case test::Mode::signed_unsigned:
		return (native ? signed_mixed_native :
			signed_mixed_software)[q][index];
	case test::Mode::unsigned_signed:
		return (native ? unsigned_mixed_native :
			unsigned_mixed_software)[q][index];
	}
	test::fail("unknown indexed signedness");
}

void require_straight_line(Gadget gadget)
{
	const auto *instructions =
		reinterpret_cast<const std::uint32_t *>(gadget);
	for (unsigned index = 0; index < 48U; ++index) {
		test::require(!conditional_branch(instructions[index]),
			"dot gadget contains a conditional branch");
		test::require((instructions[index] & 0xfc00'0000U) != 0x9400'0000U,
			"dot gadget contains a helper call");
		if (indirect_branch(instructions[index]))
			return;
	}
	test::fail("dot gadget did not reach indirect dispatch");
}

void add_every_entry(std::set<std::uintptr_t> &entries)
{
#define UML_ADD_VECTOR(name, shape) \
	entries.insert(address(uml_arm64_gadget_simd_dot_vector_##name##_##shape)); \
	entries.insert(address( \
		uml_arm64_gadget_simd_dot_vector_##name##_##shape##_soft));
#define UML_ADD_VECTOR_FAMILY(name) \
	UML_ADD_VECTOR(name, 2s) \
	UML_ADD_VECTOR(name, 4s)
	UML_ADD_VECTOR_FAMILY(sdot)
	UML_ADD_VECTOR_FAMILY(udot)
	UML_ADD_VECTOR_FAMILY(usdot)
#undef UML_ADD_VECTOR_FAMILY
#undef UML_ADD_VECTOR

#define UML_ADD_INDEXED_ONE(name, shape, index) \
	entries.insert(address( \
		uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_##index)); \
	entries.insert(address( \
		uml_arm64_gadget_simd_dot_indexed_##name##_##shape##_##index##_soft));
#define UML_ADD_INDEXED_SHAPE(name, shape) \
	UML_ADD_INDEXED_ONE(name, shape, 0) \
	UML_ADD_INDEXED_ONE(name, shape, 1) \
	UML_ADD_INDEXED_ONE(name, shape, 2) \
	UML_ADD_INDEXED_ONE(name, shape, 3)
#define UML_ADD_INDEXED_FAMILY(name) \
	UML_ADD_INDEXED_SHAPE(name, 2s) \
	UML_ADD_INDEXED_SHAPE(name, 4s)
	UML_ADD_INDEXED_FAMILY(sdot)
	UML_ADD_INDEXED_FAMILY(udot)
	UML_ADD_INDEXED_FAMILY(sudot)
	UML_ADD_INDEXED_FAMILY(usdot)
#undef UML_ADD_INDEXED_FAMILY
#undef UML_ADD_INDEXED_SHAPE
#undef UML_ADD_INDEXED_ONE
}

#undef UML_DOT_INDEXED_SOFT_FAMILY
#undef UML_DOT_INDEXED_FAMILY
#undef UML_DOT_INDEXED_SOFT_SHAPE
#undef UML_DOT_INDEXED_SHAPE

void translation_specializes_every_form()
{
	constexpr auto base = test::engine_tests::engine_test_code_base;
	constexpr test::Registers registers{31U, 30U, 29U};
	std::set<std::uintptr_t> selected;
	for (const auto mode : test::vector_modes)
		for (const bool q : {false, true}) {
			const auto built = gadgets::Builder{}.build(base,
				std::array{test::instruction(
					test::Form::vector, mode, q, registers)});
			const auto words = built.program.words();
			test::require(built.complete && words.size() == 4U,
				"dot vector stream has an unexpected shape");
			test::require(words[0] == address(vector_gadget(mode, q)),
				"dot vector host/form choice was not made in translation");
			test::require(words[1] ==
					gadgets::selection::packed_u16_operands(
						vector_offset(registers.destination),
						vector_offset(registers.left),
						vector_offset(registers.right)),
				"dot vector offsets were not packed");
			selected.insert(words[0]);
		}
	for (const auto mode : test::indexed_modes)
		for (const bool q : {false, true})
			for (unsigned index = 0; index < 4U; ++index) {
				const auto built = gadgets::Builder{}.build(base,
					std::array{test::instruction(
						test::Form::indexed, mode, q,
						registers, index)});
				const auto words = built.program.words();
				test::require(built.complete && words.size() == 4U,
					"dot indexed stream has an unexpected shape");
				test::require(words[0] ==
						address(indexed_gadget(mode, q, index)),
					"dot indexed host/form choice was not made in translation");
				test::require(words[1] ==
						gadgets::selection::packed_u16_operands(
							vector_offset(registers.destination),
							vector_offset(registers.left),
							vector_offset(registers.right)),
					"dot indexed offsets were not packed");
				selected.insert(words[0]);
			}
	test::require(selected.size() == 38U,
		"dot translation has duplicate or unreachable selected entries");
}

void every_native_and_portable_entry_is_straight_line()
{
	std::set<std::uintptr_t> entries;
	add_every_entry(entries);
	test::require(entries.size() == 76U,
		"dot gadget symbol inventory is incomplete or duplicated");
	for (const auto entry : entries)
		require_straight_line(reinterpret_cast<Gadget>(entry));
}

} // namespace

int main()
{
	translation_specializes_every_form();
	every_native_and_portable_entry_is_straight_line();
	return 0;
}
