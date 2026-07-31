// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fp8_convert_up_engine_test_support.hpp"
#include "arm64_system_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "runtime/host_features.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>

namespace test = arm64_fp8_convert_up_engine_test;
namespace common = arm64_system_test;
namespace arm64 = uml::backend::arm64;
namespace gadgets = uml::backend::arm64::gadgets;

extern "C" {
void uml_arm64_gadget_fp8_software();
void uml_arm64_gadget_svc();
extern void (*uml_arm64_fp8_native_up_table[])();
}

namespace {

using Gadget = void (*)();

[[nodiscard]] std::uintptr_t address(Gadget gadget) noexcept
{
	return reinterpret_cast<std::uintptr_t>(gadget);
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
	test::Registers registers) noexcept
{
	return vector_offset(registers.destination) |
		(vector_offset(registers.source) << 16);
}

[[nodiscard]] unsigned table_index(const test::Form &form) noexcept
{
	return (form.bfloat ? 4U : 0U) +
		(form.upper ? 2U : 0U) +
		static_cast<unsigned>(form.source2);
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

void native_words_and_exact_translation()
{
	const bool native =
		uml::backend::host_arm_features().has(
			uml::backend::ArmFeature::fp8);
	for (const auto form : test::forms) {
		const auto index = table_index(form);
		const auto native_gadget =
			uml_arm64_fp8_native_up_table[index];
		common::require(contains_word(
			native_gadget,
			test::instruction(form, {0U, 1U})));
		for (const auto registers : test::register_edges) {
			const auto result = build(form, registers);
			common::require(result.complete);
			common::require(
				result.program.instruction_count() == 2U);
			const auto &words = result.program.words();
			const auto selected = native ? native_gadget :
				uml_arm64_gadget_fp8_software;
			common::require(words.front() == address(selected));
			const unsigned operand_index = native ? 1U : 2U;
			common::require(words[operand_index] ==
				expected_operand(registers));
			common::require(words[words.size() - 2U] ==
				address(uml_arm64_gadget_svc));
			common::require(words.back() ==
				test::engine_tests::engine_test_code_base + 8U);
		}
	}
}

void native_hot_path_has_no_runtime_shape_decode()
{
	for (unsigned index = 0U; index < test::forms.size(); ++index) {
		const auto gadget = uml_arm64_fp8_native_up_table[index];
		/* The two predictable branches belong only to the shared FP/FPMR
		 * environment activation. Shape, source selector, half, and
		 * register allocation have no runtime branches. */
		common::require(conditional_branch_count(gadget) == 2U);
		common::require(runtime_instruction_count(gadget) == 19U);
	}
}

} // namespace

int main()
{
	static_assert(vector_offset(31U) <= 0xffffU);
	native_words_and_exact_translation();
	native_hot_path_has_no_runtime_shape_decode();
	return 0;
}
