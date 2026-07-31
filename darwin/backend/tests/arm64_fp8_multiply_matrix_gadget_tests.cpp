// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_multiply_matrix_engine_test_support.hpp"
#include "arm64_system_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "runtime/host_features.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>

namespace test = arm64_fp8_multiply_matrix_engine_test;
namespace common = arm64_system_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

extern "C" {
void uml_arm64_gadget_fp8_software();
void uml_arm64_gadget_svc();
extern void (*uml_arm64_fp8_native_mul_h_vector_table[])();
extern void (*uml_arm64_fp8_native_mul_h_indexed_table[])();
extern void (*uml_arm64_fp8_native_mul_s_vector_table[])();
extern void (*uml_arm64_fp8_native_mul_s_indexed_table[])();
extern void (*uml_arm64_fp8_native_matrix_table[])();
}

namespace {

using Gadget = void (*)();

[[nodiscard]] std::uintptr_t address(Gadget gadget) noexcept
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] Gadget native_gadget(const test::Form &form)
{
	if (form.family == test::Family::matrix)
		return uml_arm64_fp8_native_matrix_table[
			form.width == 32U];
	if (form.width == 16U)
		return form.by_element ?
			uml_arm64_fp8_native_mul_h_indexed_table[
				form.selection * 16U + form.index] :
			uml_arm64_fp8_native_mul_h_vector_table[
				form.selection];
	return form.by_element ?
		uml_arm64_fp8_native_mul_s_indexed_table[
			form.selection * 16U + form.index] :
		uml_arm64_fp8_native_mul_s_vector_table[form.selection];
}

[[nodiscard]] bool contains_word(Gadget gadget, std::uint32_t word)
{
	const auto *bytes = reinterpret_cast<const std::byte *>(gadget);
	for (unsigned offset = 0U; offset < 128U; offset += 4U) {
		std::uint32_t candidate{};
		std::memcpy(&candidate, bytes + offset, sizeof(candidate));
		if (candidate == word)
			return true;
	}
	return false;
}

[[nodiscard]] constexpr std::uintptr_t vector_offset(unsigned reg) noexcept
{
	return offsetof(arm64::CpuState, vectors) +
		reg * sizeof(arm64::VectorRegister);
}

[[nodiscard]] constexpr std::uintptr_t expected_operand(
	const test::Form &form, test::Registers registers) noexcept
{
	return vector_offset(registers.destination) |
		(vector_offset(registers.first) << 16U) |
		(vector_offset(registers.second) << 32U) |
		(static_cast<std::uintptr_t>(
			form.by_element ? form.index : 0U) << 48U);
}

[[nodiscard]] gadgets::BuildResult build(
	const test::Form &form, test::Registers registers)
{
	const std::array code{
		test::instruction(form, registers),
		std::uint32_t{0xd400'0001U},
	};
	return gadgets::Builder{}.build(
		test::engine_tests::engine_test_code_base, code, {},
		std::pmr::get_default_resource());
}

[[nodiscard]] unsigned runtime_instruction_count(Gadget gadget)
{
	const auto *instructions =
		reinterpret_cast<const std::uint32_t *>(address(gadget));
	for (unsigned index = 0U; index < 32U; ++index)
		if ((instructions[index] & 0xffff'fc1fU) == 0xd61f'0000U)
			return index + 1U;
	common::require(false);
	return 0U;
}

[[nodiscard]] unsigned conditional_branch_count(Gadget gadget)
{
	const auto *instructions =
		reinterpret_cast<const std::uint32_t *>(address(gadget));
	unsigned branches{};
	for (unsigned index = 0U; index < 32U; ++index) {
		const auto instruction = instructions[index];
		if ((instruction & 0xffff'fc1fU) == 0xd61f'0000U)
			return branches;
		if ((instruction & 0xff00'0010U) == 0x5400'0000U ||
		    (instruction & 0x7e00'0000U) == 0x3400'0000U ||
		    (instruction & 0x7e00'0000U) == 0x3600'0000U)
			++branches;
	}
	common::require(false);
	return 0U;
}

template<typename Function>
void for_each_form(Function &&function)
{
	test::for_each_multiply_form(function);
	test::for_each_matrix_form(function);
}

[[nodiscard]] bool use_native(const test::Form &form)
{
	using uml::backend::ArmFeature;
	const auto &features = uml::backend::host_arm_features();
	const auto operation_feature =
		form.family == test::Family::multiply ?
			ArmFeature::fp8fma :
		form.width == 16U ? ArmFeature::f8f16mm :
			ArmFeature::f8f32mm;
	return features.has(operation_feature) &&
		features.has(ArmFeature::afp);
}

void native_words_direct_operands_and_selected_path()
{
	for_each_form([&](const test::Form &form) {
		const auto native = native_gadget(form);
		common::require(contains_word(native,
			test::instruction(form, {0U, 1U, 2U})));
		for (const auto registers : test::register_aliases) {
			if (form.by_element && registers.second >= 8U)
				continue;
			const auto result = build(form, registers);
			common::require(result.complete);
			common::require(
				result.program.instruction_count() == 2U);
			const auto &words = result.program.words();
			common::require(words.front() == address(
				use_native(form) ? native :
					uml_arm64_gadget_fp8_software));
			const unsigned operand_index =
				use_native(form) ? 1U : 2U;
			common::require(words[operand_index] ==
				expected_operand(form, registers));
			common::require(words[words.size() - 2U] ==
				address(uml_arm64_gadget_svc));
			common::require(words.back() ==
				test::engine_tests::engine_test_code_base + 8U);
		}
	});
}

void native_hot_paths_have_no_shape_or_register_branch()
{
	for_each_form([&](const test::Form &form) {
		const auto gadget = native_gadget(form);
		common::require(conditional_branch_count(gadget) == 2U);
		common::require(runtime_instruction_count(gadget) == 23U);
	});
}

} // namespace

int main()
{
	static_assert(vector_offset(31U) <= 0xffffU);
	native_words_direct_operands_and_selected_path();
	native_hot_paths_have_no_shape_or_register_branch();
	return 0;
}
