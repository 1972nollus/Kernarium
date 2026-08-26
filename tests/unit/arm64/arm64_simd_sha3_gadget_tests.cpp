// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_sha3_test_support.hpp"

#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "platform/apple/host_features.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

extern "C" {
void uml_arm64_gadget_simd_eor3_native();
void uml_arm64_gadget_simd_eor3_soft();
void uml_arm64_gadget_simd_bcax_native();
void uml_arm64_gadget_simd_bcax_soft();
void uml_arm64_gadget_simd_rax1_native();
void uml_arm64_gadget_simd_rax1_soft();
#define UML_DECLARE_XAR(rotate) \
	void uml_arm64_gadget_simd_xar_native_##rotate(); \
	void uml_arm64_gadget_simd_xar_soft_##rotate();
UML_DECLARE_XAR(0) UML_DECLARE_XAR(1)
UML_DECLARE_XAR(2) UML_DECLARE_XAR(3)
UML_DECLARE_XAR(4) UML_DECLARE_XAR(5)
UML_DECLARE_XAR(6) UML_DECLARE_XAR(7)
UML_DECLARE_XAR(8) UML_DECLARE_XAR(9)
UML_DECLARE_XAR(10) UML_DECLARE_XAR(11)
UML_DECLARE_XAR(12) UML_DECLARE_XAR(13)
UML_DECLARE_XAR(14) UML_DECLARE_XAR(15)
UML_DECLARE_XAR(16) UML_DECLARE_XAR(17)
UML_DECLARE_XAR(18) UML_DECLARE_XAR(19)
UML_DECLARE_XAR(20) UML_DECLARE_XAR(21)
UML_DECLARE_XAR(22) UML_DECLARE_XAR(23)
UML_DECLARE_XAR(24) UML_DECLARE_XAR(25)
UML_DECLARE_XAR(26) UML_DECLARE_XAR(27)
UML_DECLARE_XAR(28) UML_DECLARE_XAR(29)
UML_DECLARE_XAR(30) UML_DECLARE_XAR(31)
UML_DECLARE_XAR(32) UML_DECLARE_XAR(33)
UML_DECLARE_XAR(34) UML_DECLARE_XAR(35)
UML_DECLARE_XAR(36) UML_DECLARE_XAR(37)
UML_DECLARE_XAR(38) UML_DECLARE_XAR(39)
UML_DECLARE_XAR(40) UML_DECLARE_XAR(41)
UML_DECLARE_XAR(42) UML_DECLARE_XAR(43)
UML_DECLARE_XAR(44) UML_DECLARE_XAR(45)
UML_DECLARE_XAR(46) UML_DECLARE_XAR(47)
UML_DECLARE_XAR(48) UML_DECLARE_XAR(49)
UML_DECLARE_XAR(50) UML_DECLARE_XAR(51)
UML_DECLARE_XAR(52) UML_DECLARE_XAR(53)
UML_DECLARE_XAR(54) UML_DECLARE_XAR(55)
UML_DECLARE_XAR(56) UML_DECLARE_XAR(57)
UML_DECLARE_XAR(58) UML_DECLARE_XAR(59)
UML_DECLARE_XAR(60) UML_DECLARE_XAR(61)
UML_DECLARE_XAR(62) UML_DECLARE_XAR(63)
#undef UML_DECLARE_XAR
}

namespace test = arm64_simd_sha3_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using Gadget = void (*)();

#define UML_XAR_GADGETS(implementation) std::array<Gadget, 64>{ \
	uml_arm64_gadget_simd_xar_##implementation##_0, \
	uml_arm64_gadget_simd_xar_##implementation##_1, \
	uml_arm64_gadget_simd_xar_##implementation##_2, \
	uml_arm64_gadget_simd_xar_##implementation##_3, \
	uml_arm64_gadget_simd_xar_##implementation##_4, \
	uml_arm64_gadget_simd_xar_##implementation##_5, \
	uml_arm64_gadget_simd_xar_##implementation##_6, \
	uml_arm64_gadget_simd_xar_##implementation##_7, \
	uml_arm64_gadget_simd_xar_##implementation##_8, \
	uml_arm64_gadget_simd_xar_##implementation##_9, \
	uml_arm64_gadget_simd_xar_##implementation##_10, \
	uml_arm64_gadget_simd_xar_##implementation##_11, \
	uml_arm64_gadget_simd_xar_##implementation##_12, \
	uml_arm64_gadget_simd_xar_##implementation##_13, \
	uml_arm64_gadget_simd_xar_##implementation##_14, \
	uml_arm64_gadget_simd_xar_##implementation##_15, \
	uml_arm64_gadget_simd_xar_##implementation##_16, \
	uml_arm64_gadget_simd_xar_##implementation##_17, \
	uml_arm64_gadget_simd_xar_##implementation##_18, \
	uml_arm64_gadget_simd_xar_##implementation##_19, \
	uml_arm64_gadget_simd_xar_##implementation##_20, \
	uml_arm64_gadget_simd_xar_##implementation##_21, \
	uml_arm64_gadget_simd_xar_##implementation##_22, \
	uml_arm64_gadget_simd_xar_##implementation##_23, \
	uml_arm64_gadget_simd_xar_##implementation##_24, \
	uml_arm64_gadget_simd_xar_##implementation##_25, \
	uml_arm64_gadget_simd_xar_##implementation##_26, \
	uml_arm64_gadget_simd_xar_##implementation##_27, \
	uml_arm64_gadget_simd_xar_##implementation##_28, \
	uml_arm64_gadget_simd_xar_##implementation##_29, \
	uml_arm64_gadget_simd_xar_##implementation##_30, \
	uml_arm64_gadget_simd_xar_##implementation##_31, \
	uml_arm64_gadget_simd_xar_##implementation##_32, \
	uml_arm64_gadget_simd_xar_##implementation##_33, \
	uml_arm64_gadget_simd_xar_##implementation##_34, \
	uml_arm64_gadget_simd_xar_##implementation##_35, \
	uml_arm64_gadget_simd_xar_##implementation##_36, \
	uml_arm64_gadget_simd_xar_##implementation##_37, \
	uml_arm64_gadget_simd_xar_##implementation##_38, \
	uml_arm64_gadget_simd_xar_##implementation##_39, \
	uml_arm64_gadget_simd_xar_##implementation##_40, \
	uml_arm64_gadget_simd_xar_##implementation##_41, \
	uml_arm64_gadget_simd_xar_##implementation##_42, \
	uml_arm64_gadget_simd_xar_##implementation##_43, \
	uml_arm64_gadget_simd_xar_##implementation##_44, \
	uml_arm64_gadget_simd_xar_##implementation##_45, \
	uml_arm64_gadget_simd_xar_##implementation##_46, \
	uml_arm64_gadget_simd_xar_##implementation##_47, \
	uml_arm64_gadget_simd_xar_##implementation##_48, \
	uml_arm64_gadget_simd_xar_##implementation##_49, \
	uml_arm64_gadget_simd_xar_##implementation##_50, \
	uml_arm64_gadget_simd_xar_##implementation##_51, \
	uml_arm64_gadget_simd_xar_##implementation##_52, \
	uml_arm64_gadget_simd_xar_##implementation##_53, \
	uml_arm64_gadget_simd_xar_##implementation##_54, \
	uml_arm64_gadget_simd_xar_##implementation##_55, \
	uml_arm64_gadget_simd_xar_##implementation##_56, \
	uml_arm64_gadget_simd_xar_##implementation##_57, \
	uml_arm64_gadget_simd_xar_##implementation##_58, \
	uml_arm64_gadget_simd_xar_##implementation##_59, \
	uml_arm64_gadget_simd_xar_##implementation##_60, \
	uml_arm64_gadget_simd_xar_##implementation##_61, \
	uml_arm64_gadget_simd_xar_##implementation##_62, \
	uml_arm64_gadget_simd_xar_##implementation##_63}

constexpr auto native_xar = UML_XAR_GADGETS(native);
constexpr auto software_xar = UML_XAR_GADGETS(soft);
#undef UML_XAR_GADGETS

[[nodiscard]] std::uintptr_t address(Gadget gadget) noexcept
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] constexpr std::uintptr_t vector_offset(unsigned index) noexcept
{
	return offsetof(test::arm64::CpuState, vectors) +
		index * sizeof(test::arm64::VectorRegister);
}

[[nodiscard]] Gadget expected_gadget(
	test::Operation operation, bool native, unsigned rotate = 0)
{
	switch (operation) {
	case test::Operation::three_way_exclusive:
		return native ? uml_arm64_gadget_simd_eor3_native :
			uml_arm64_gadget_simd_eor3_soft;
	case test::Operation::clear_and_exclusive:
		return native ? uml_arm64_gadget_simd_bcax_native :
			uml_arm64_gadget_simd_bcax_soft;
	case test::Operation::rotate_one_and_exclusive:
		return native ? uml_arm64_gadget_simd_rax1_native :
			uml_arm64_gadget_simd_rax1_soft;
	case test::Operation::exclusive_and_rotate:
		return (native ? native_xar : software_xar)[rotate];
	}
	return nullptr;
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

[[nodiscard]] unsigned instruction_count(Gadget gadget)
{
	const auto *instructions =
		reinterpret_cast<const std::uint32_t *>(gadget);
	for (unsigned index = 0; index < 32U; ++index)
		if (indirect_branch(instructions[index]))
			return index + 1U;
	test::fail("logical/rotation gadget did not reach threaded dispatch");
}

[[nodiscard]] unsigned conditional_branches(Gadget gadget)
{
	const auto *instructions =
		reinterpret_cast<const std::uint32_t *>(gadget);
	unsigned branches = 0;
	for (unsigned index = 0; index < 32U; ++index) {
		if (indirect_branch(instructions[index]))
			return branches;
		if (conditional_branch(instructions[index]))
			++branches;
	}
	test::fail("logical/rotation gadget did not reach threaded dispatch");
}

void translation_specializes_operation_immediate_and_registers()
{
	const bool native =
		::uml::platform::apple::probed_host_capabilities().has(
			::uml::cpu::ArmFeature::sha3);
	constexpr test::Registers registers{31, 30, 29, 28};
	std::set<std::uintptr_t> selected;
	for (const auto operation : {
		     test::Operation::three_way_exclusive,
		     test::Operation::clear_and_exclusive,
		     test::Operation::rotate_one_and_exclusive,
	     }) {
		const test::Variant variant{operation, registers, 0};
		const auto built = gadgets::Builder{}.build(
			test::engine_tests::engine_test_code_base,
			std::array{test::instruction(variant)});
		const auto words = built.program.words();
		test::require(built.complete && words.size() == 4U,
			"logical/rotation gadget stream is truncated");
		const auto gadget = expected_gadget(operation, native);
		test::require(words[0] == address(gadget),
			"operation or host path remained in the runtime stream");
		const auto third = test::four_sources(operation) ?
			vector_offset(registers.third) : 0U;
		test::require(words[1] ==
				gadgets::selection::packed_u16_operands(
					vector_offset(registers.destination),
					vector_offset(registers.first),
					vector_offset(registers.second), third),
			"register offsets were not packed during translation");
		selected.insert(words[0]);
	}
	for (unsigned rotate = 0; rotate < 64U; ++rotate) {
		const test::Variant variant{
			test::Operation::exclusive_and_rotate,
			registers,
			rotate,
		};
		const auto built = gadgets::Builder{}.build(
			test::engine_tests::engine_test_code_base,
			std::array{test::instruction(variant)});
		const auto words = built.program.words();
		test::require(built.complete && words.size() == 4U,
			"immediate rotation gadget stream is truncated");
		test::require(words[0] == address(
				expected_gadget(variant.operation, native, rotate)),
			"rotation immediate remained in the runtime stream");
		test::require(words[1] ==
				gadgets::selection::packed_u16_operands(
					vector_offset(registers.destination),
					vector_offset(registers.first),
					vector_offset(registers.second)),
			"rotation register offsets were not packed");
		selected.insert(words[0]);
	}
	test::require(selected.size() == 67U,
		"translation did not select every current-host gadget");
}

void every_definition_is_reachable_and_branch_free()
{
	std::set<std::uintptr_t> definitions;
	unsigned maximum_native = 0;
	unsigned maximum_software = 0;
	for (const auto operation : {
		     test::Operation::three_way_exclusive,
		     test::Operation::clear_and_exclusive,
		     test::Operation::rotate_one_and_exclusive,
	     })
		for (const bool native : {false, true}) {
			const auto gadget = expected_gadget(operation, native);
			definitions.insert(address(gadget));
			test::require(conditional_branches(gadget) == 0U,
				"valid gadget contains a conditional branch");
			auto &maximum = native ?
				maximum_native : maximum_software;
			maximum = std::max(maximum,
				instruction_count(gadget));
		}
	for (unsigned rotate = 0; rotate < 64U; ++rotate)
		for (const bool native : {false, true}) {
			const auto gadget = expected_gadget(
				test::Operation::exclusive_and_rotate,
				native, rotate);
			definitions.insert(address(gadget));
			test::require(conditional_branches(gadget) == 0U,
				"rotation gadget contains a conditional branch");
			auto &maximum = native ?
				maximum_native : maximum_software;
			maximum = std::max(maximum,
				instruction_count(gadget));
		}
	test::require(definitions.size() == 134U,
		"logical/rotation gadget table has a duplicate or missing entry");
	test::require(maximum_native <= 12U && maximum_software <= 14U,
		"logical/rotation hot path exceeds its instruction budget");
}

} // namespace

int main()
{
	translation_specializes_operation_immediate_and_registers();
	every_definition_is_reachable_and_branch_free();
	return 0;
}
