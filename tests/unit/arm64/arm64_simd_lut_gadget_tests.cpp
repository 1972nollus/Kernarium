// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_lut_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "platform/apple/host_features.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>

namespace uml::emulator::tests::simd_lut {
namespace {

namespace gadgets = arm64::gadgets;
using Gadget = void (*)();

extern "C" {
#define UML_DECLARE_LUT_PART(name, part) \
	void uml_arm64_gadget_simd_lut_native_##name##_##part();
UML_DECLARE_LUT_PART(luti2_b, 0)
UML_DECLARE_LUT_PART(luti2_b, 1)
UML_DECLARE_LUT_PART(luti2_b, 2)
UML_DECLARE_LUT_PART(luti2_b, 3)
UML_DECLARE_LUT_PART(luti2_h, 0)
UML_DECLARE_LUT_PART(luti2_h, 1)
UML_DECLARE_LUT_PART(luti2_h, 2)
UML_DECLARE_LUT_PART(luti2_h, 3)
UML_DECLARE_LUT_PART(luti2_h, 4)
UML_DECLARE_LUT_PART(luti2_h, 5)
UML_DECLARE_LUT_PART(luti2_h, 6)
UML_DECLARE_LUT_PART(luti2_h, 7)
UML_DECLARE_LUT_PART(luti4_b, 0)
UML_DECLARE_LUT_PART(luti4_b, 1)
UML_DECLARE_LUT_PART(luti4_h, 0)
UML_DECLARE_LUT_PART(luti4_h, 1)
UML_DECLARE_LUT_PART(luti4_h, 2)
UML_DECLARE_LUT_PART(luti4_h, 3)
#undef UML_DECLARE_LUT_PART
void uml_arm64_gadget_simd_lut_software();
void uml_arm64_gadget_illegal();
void uml_arm64_gadget_svc();
}

constexpr std::array<Gadget, 4> luti2_b{
	uml_arm64_gadget_simd_lut_native_luti2_b_0,
	uml_arm64_gadget_simd_lut_native_luti2_b_1,
	uml_arm64_gadget_simd_lut_native_luti2_b_2,
	uml_arm64_gadget_simd_lut_native_luti2_b_3,
};
constexpr std::array<Gadget, 8> luti2_h{
	uml_arm64_gadget_simd_lut_native_luti2_h_0,
	uml_arm64_gadget_simd_lut_native_luti2_h_1,
	uml_arm64_gadget_simd_lut_native_luti2_h_2,
	uml_arm64_gadget_simd_lut_native_luti2_h_3,
	uml_arm64_gadget_simd_lut_native_luti2_h_4,
	uml_arm64_gadget_simd_lut_native_luti2_h_5,
	uml_arm64_gadget_simd_lut_native_luti2_h_6,
	uml_arm64_gadget_simd_lut_native_luti2_h_7,
};
constexpr std::array<Gadget, 2> luti4_b{
	uml_arm64_gadget_simd_lut_native_luti4_b_0,
	uml_arm64_gadget_simd_lut_native_luti4_b_1,
};
constexpr std::array<Gadget, 4> luti4_h{
	uml_arm64_gadget_simd_lut_native_luti4_h_0,
	uml_arm64_gadget_simd_lut_native_luti4_h_1,
	uml_arm64_gadget_simd_lut_native_luti4_h_2,
	uml_arm64_gadget_simd_lut_native_luti4_h_3,
};

[[nodiscard]] std::uintptr_t address(Gadget gadget) noexcept
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] Gadget native_gadget(const Form &form)
{
	if (form.index_bits == 2U && form.element_bytes == 1U)
		return luti2_b[form.part];
	if (form.index_bits == 2U && form.element_bytes == 2U)
		return luti2_h[form.part];
	if (form.index_bits == 4U && form.element_bytes == 1U)
		return luti4_b[form.part];
	if (form.index_bits == 4U && form.element_bytes == 2U)
		return luti4_h[form.part];
	arm64_system_test::require(false);
	return nullptr;
}

[[nodiscard]] constexpr std::size_t vector_offset(unsigned reg) noexcept
{
	return offsetof(arm64::CpuState, vectors) +
		reg * sizeof(arm64::VectorRegister);
}

[[nodiscard]] constexpr std::uintptr_t expected_operand(
	const Form &form, unsigned indices, unsigned table,
	unsigned destination) noexcept
{
	return vector_offset(destination) |
		(static_cast<std::uintptr_t>(vector_offset(table)) << 11U) |
		(static_cast<std::uintptr_t>(
			vector_offset((table + 1U) & 31U)) << 22U) |
		(static_cast<std::uintptr_t>(vector_offset(indices)) << 33U) |
		(static_cast<std::uintptr_t>(
			form.index_bits == 2U ?
				(form.element_bytes == 1U ? 0U : 1U) :
				(form.element_bytes == 1U ? 2U : 3U))
		 << 44U) |
		(static_cast<std::uintptr_t>(form.part) << 46U);
}

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	const std::array code{instruction, terminal_system_call};
	return gadgets::Builder{}.build(engine_test_code_base, code, {},
		std::pmr::get_default_resource());
}

[[nodiscard]] unsigned runtime_instruction_count(Gadget gadget)
{
	const auto *instructions =
		reinterpret_cast<const std::uint32_t *>(address(gadget));
	for (unsigned index = 0; index < 24U; ++index)
		if ((instructions[index] & 0xffff'fc1fU) == 0xd61f'0000U)
			return index + 1U;
	arm64_system_test::require(false);
	return 0;
}

[[nodiscard]] unsigned conditional_branch_count(Gadget gadget)
{
	const auto *instructions =
		reinterpret_cast<const std::uint32_t *>(address(gadget));
	unsigned branches = 0;
	for (unsigned index = 0; index < 24U; ++index) {
		const auto instruction = instructions[index];
		if ((instruction & 0xffff'fc1fU) == 0xd61f'0000U)
			return branches;
		if ((instruction & 0xff00'0010U) == 0x5400'0000U ||
		    (instruction & 0x7e00'0000U) == 0x3400'0000U ||
		    (instruction & 0x7e00'0000U) == 0x3600'0000U)
			++branches;
	}
	arm64_system_test::require(false);
	return 0;
}

void exact_translation()
{
	const bool native =
		::uml::platform::apple::probed_host_capabilities().has(ArmFeature::advanced_simd) &&
		::uml::platform::apple::probed_host_capabilities().has(ArmFeature::lut);
	for (const auto &form : forms)
		for (unsigned role = 0; role < 3U; ++role)
			for (unsigned reg = 0; reg < 32U; ++reg) {
				const unsigned destination =
					role == 0U ? reg : 3U;
				const unsigned table =
					role == 1U ? reg : 11U;
				const unsigned indices =
					role == 2U ? reg : 23U;
				const auto result = build(encode(form,
					indices, table, destination));
				arm64_system_test::require(result.complete);
				arm64_system_test::require(
					result.program.instruction_count() == 2U);
				arm64_system_test::require(
					result.program.words().size() == 4U);
				arm64_system_test::require(
					result.program.words()[0] ==
						address(native ?
							native_gadget(form) :
							uml_arm64_gadget_simd_lut_software));
				arm64_system_test::require(
					result.program.words()[1] ==
						expected_operand(form, indices,
							table, destination));
				arm64_system_test::require(
					result.program.words()[2] ==
						address(uml_arm64_gadget_svc));
				arm64_system_test::require(
					result.program.words()[3] ==
						engine_test_code_base + 8U);
			}

	for (unsigned op2 = 1U; op2 <= 3U; ++op2)
		for (unsigned len = 0; len < 4U; ++len)
			for (unsigned op = 0; op < 2U; ++op) {
				const auto instruction =
					encode(op2, len, op, 23U, 11U, 3U);
				if (decode(instruction).valid)
					continue;
				const auto result = build(instruction);
				arm64_system_test::require(result.complete);
				arm64_system_test::require(
					result.program.instruction_count() == 1U);
				arm64_system_test::require(
					result.program.words().size() == 2U);
				arm64_system_test::require(
					result.program.words()[0] ==
						address(uml_arm64_gadget_illegal));
				arm64_system_test::require(
					result.program.words()[1] ==
						engine_test_code_base);
			}
}

void hot_path_shape()
{
	for (const auto &form : forms) {
		const auto gadget = native_gadget(form);
		arm64_system_test::require(
			conditional_branch_count(gadget) == 0U);
		arm64_system_test::require(
			runtime_instruction_count(gadget) ==
				(form.table_count == 2U ? 12U : 10U));
	}
	arm64_system_test::require(
		conditional_branch_count(
			uml_arm64_gadget_simd_lut_software) == 0U);
	arm64_system_test::require(
		runtime_instruction_count(
			uml_arm64_gadget_simd_lut_software) == 8U);
}

} // namespace
} // namespace uml::emulator::tests::simd_lut

int main()
{
	uml::emulator::tests::simd_lut::exact_translation();
	uml::emulator::tests::simd_lut::hot_path_shape();
	return 0;
}
