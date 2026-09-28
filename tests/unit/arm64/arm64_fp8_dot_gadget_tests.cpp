// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_fp8_dot_engine_test_support.hpp"
#include "arm64_system_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "platform/apple/host_features.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>

using uml::testing::a64::conditional_branch;
using uml::testing::a64::indirect_branch;

namespace test = arm64_fp8_dot_engine_test;
namespace common = arm64_system_test;
namespace arm64 = uml::cpu::arm64;
namespace gadgets = uml::cpu::arm64::gadgets;

extern "C" {
void uml_arm64_gadget_fp8_software();
void uml_arm64_gadget_svc();
extern void (*uml_arm64_fp8_native_dot_h_vector_table[])();
extern void (*uml_arm64_fp8_native_dot_h_indexed_table[])();
extern void (*uml_arm64_fp8_native_dot_s_vector_table[])();
extern void (*uml_arm64_fp8_native_dot_s_indexed_table[])();
}

namespace {

using Gadget = void (*)();

[[nodiscard]] std::uintptr_t address(Gadget gadget) noexcept
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] Gadget native_gadget(const test::Form &form)
{
	const unsigned q = static_cast<unsigned>(form.wide);
	if (form.width == 16U)
		return form.by_element ?
			uml_arm64_fp8_native_dot_h_indexed_table[
				q * 8U + form.index] :
			uml_arm64_fp8_native_dot_h_vector_table[q];
	return form.by_element ?
		uml_arm64_fp8_native_dot_s_indexed_table[
			q * 4U + form.index] :
		uml_arm64_fp8_native_dot_s_vector_table[q];
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
		if (indirect_branch(instructions[index]))
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
		if (indirect_branch(instruction))
			return branches;
		if (conditional_branch(instruction))
			++branches;
	}
	common::require(false);
	return 0U;
}

template<typename Function>
void for_each_form(Function &&function)
{
	for (const unsigned width : {16U, 32U})
		for (const bool wide : {false, true}) {
			function(test::Form{width, false, wide, 0U});
			const unsigned indices = width == 16U ? 8U : 4U;
			for (unsigned index = 0U; index < indices; ++index)
				function(test::Form{width, true, wide, index});
		}
}

void native_words_direct_operands_and_selected_path()
{
	for_each_form([&](const test::Form &form) {
		const auto native = native_gadget(form);
		common::require(contains_word(native,
			test::instruction(form, {0U, 1U, 2U})));
		const auto feature = form.width == 16U ?
			::uml::cpu::ArmFeature::fp8dot2 :
			::uml::cpu::ArmFeature::fp8dot4;
		const bool use_native =
			::uml::platform::apple::probed_host_capabilities().has(feature) &&
			::uml::platform::apple::probed_host_capabilities().has(
				::uml::cpu::ArmFeature::afp);
		for (const auto registers : test::register_aliases) {
			if (form.by_element && form.width == 16U &&
			    registers.second >= 16U)
				continue;
			const auto result = build(form, registers);
			common::require(result.complete);
			common::require(
				result.program.instruction_count() == 2U);
			const auto &words = result.program.words();
			common::require(words.front() == address(
				use_native ? native :
					uml_arm64_gadget_fp8_software));
			const unsigned operand_index =
				use_native ? 1U : 2U;
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
