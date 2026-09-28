// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_base_pstate_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>

using uml::testing::a64::conditional_branch;
using uml::testing::a64::indirect_branch;

namespace uml::emulator::tests::base_pstate {
namespace {

namespace gadgets = arm64::gadgets;
using Gadget = void (*)();

extern "C" {
#define UML_DECLARE_PSTATE_FIELD(name) \
	void uml_arm64_gadget_pstate_##name##_0(); \
	void uml_arm64_gadget_pstate_##name##_1();
UML_DECLARE_PSTATE_FIELD(ssbs)
UML_DECLARE_PSTATE_FIELD(dit)
UML_DECLARE_PSTATE_FIELD(tco)
UML_DECLARE_PSTATE_FIELD(sm)
UML_DECLARE_PSTATE_FIELD(za)
UML_DECLARE_PSTATE_FIELD(smza)
#undef UML_DECLARE_PSTATE_FIELD
#define UML_DECLARE_PSTATE_DAIF(name, immediate) \
	void uml_arm64_gadget_pstate_daif_##name##_##immediate();
#define UML_DECLARE_PSTATE_DAIF_IMMEDIATE(immediate) \
	UML_DECLARE_PSTATE_DAIF(set, immediate) \
	UML_DECLARE_PSTATE_DAIF(clear, immediate)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(0)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(1)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(2)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(3)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(4)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(5)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(6)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(7)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(8)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(9)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(10)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(11)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(12)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(13)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(14)
UML_DECLARE_PSTATE_DAIF_IMMEDIATE(15)
#undef UML_DECLARE_PSTATE_DAIF_IMMEDIATE
#undef UML_DECLARE_PSTATE_DAIF
void uml_arm64_gadget_illegal();
void uml_arm64_gadget_svc();
}

constexpr std::array<Gadget, 2> ssbs_gadgets{
	uml_arm64_gadget_pstate_ssbs_0,
	uml_arm64_gadget_pstate_ssbs_1,
};
constexpr std::array<Gadget, 2> dit_gadgets{
	uml_arm64_gadget_pstate_dit_0,
	uml_arm64_gadget_pstate_dit_1,
};
constexpr std::array<Gadget, 2> tco_gadgets{
	uml_arm64_gadget_pstate_tco_0,
	uml_arm64_gadget_pstate_tco_1,
};
constexpr std::array<Gadget, 2> sm_gadgets{
	uml_arm64_gadget_pstate_sm_0,
	uml_arm64_gadget_pstate_sm_1,
};
constexpr std::array<Gadget, 2> za_gadgets{
	uml_arm64_gadget_pstate_za_0,
	uml_arm64_gadget_pstate_za_1,
};
constexpr std::array<Gadget, 2> smza_gadgets{
	uml_arm64_gadget_pstate_smza_0,
	uml_arm64_gadget_pstate_smza_1,
};

constexpr std::array<Gadget, 16> daif_set_gadgets{
	uml_arm64_gadget_pstate_daif_set_0,
	uml_arm64_gadget_pstate_daif_set_1,
	uml_arm64_gadget_pstate_daif_set_2,
	uml_arm64_gadget_pstate_daif_set_3,
	uml_arm64_gadget_pstate_daif_set_4,
	uml_arm64_gadget_pstate_daif_set_5,
	uml_arm64_gadget_pstate_daif_set_6,
	uml_arm64_gadget_pstate_daif_set_7,
	uml_arm64_gadget_pstate_daif_set_8,
	uml_arm64_gadget_pstate_daif_set_9,
	uml_arm64_gadget_pstate_daif_set_10,
	uml_arm64_gadget_pstate_daif_set_11,
	uml_arm64_gadget_pstate_daif_set_12,
	uml_arm64_gadget_pstate_daif_set_13,
	uml_arm64_gadget_pstate_daif_set_14,
	uml_arm64_gadget_pstate_daif_set_15,
};
constexpr std::array<Gadget, 16> daif_clear_gadgets{
	uml_arm64_gadget_pstate_daif_clear_0,
	uml_arm64_gadget_pstate_daif_clear_1,
	uml_arm64_gadget_pstate_daif_clear_2,
	uml_arm64_gadget_pstate_daif_clear_3,
	uml_arm64_gadget_pstate_daif_clear_4,
	uml_arm64_gadget_pstate_daif_clear_5,
	uml_arm64_gadget_pstate_daif_clear_6,
	uml_arm64_gadget_pstate_daif_clear_7,
	uml_arm64_gadget_pstate_daif_clear_8,
	uml_arm64_gadget_pstate_daif_clear_9,
	uml_arm64_gadget_pstate_daif_clear_10,
	uml_arm64_gadget_pstate_daif_clear_11,
	uml_arm64_gadget_pstate_daif_clear_12,
	uml_arm64_gadget_pstate_daif_clear_13,
	uml_arm64_gadget_pstate_daif_clear_14,
	uml_arm64_gadget_pstate_daif_clear_15,
};

[[nodiscard]] std::uintptr_t address(Gadget gadget) noexcept
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] Gadget expected_gadget(
	const Decoded &decoded, unsigned immediate)
{
	const auto value = immediate & 1U;
	if (!decoded.allocated || !decoded.el0_accessible)
		return uml_arm64_gadget_illegal;
	switch (decoded.field) {
	case Field::ssbs:
		return ssbs_gadgets[value];
	case Field::dit:
		return dit_gadgets[value];
	case Field::streaming_mode:
		return sm_gadgets[value];
	case Field::array_state:
		return za_gadgets[value];
	case Field::streaming_and_array_state:
		return smza_gadgets[value];
	case Field::tag_check_override:
		return tco_gadgets[value];
	case Field::interrupt_mask_set:
		return daif_set_gadgets[immediate];
	case Field::interrupt_mask_clear:
		return daif_clear_gadgets[immediate];
	case Field::flag_alias:
	case Field::reserved:
	case Field::uao:
	case Field::pan:
	case Field::stack_pointer_select:
	case Field::all_interrupt_mask:
	case Field::profiling_mask:
		break;
	}
	return uml_arm64_gadget_illegal;
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
	for (unsigned index = 0; index < 20U; ++index)
		if (indirect_branch(instructions[index]))
			return index + 1U;
	arm64_system_test::require(false);
	return 0;
}

[[nodiscard]] unsigned conditional_branch_count(Gadget gadget)
{
	const auto *instructions =
		reinterpret_cast<const std::uint32_t *>(address(gadget));
	unsigned branches = 0;
	for (unsigned index = 0; index < 20U; ++index) {
		const auto instruction = instructions[index];
		if (indirect_branch(instruction))
			return branches;
		if (conditional_branch(instruction))
			++branches;
	}
	arm64_system_test::require(false);
	return 0;
}

void exhaustive_translation()
{
	for (unsigned op1 = 0; op1 < 8U; ++op1)
		for (unsigned op2 = 0; op2 < 8U; ++op2)
			for (unsigned immediate = 0; immediate < 16U; ++immediate) {
				const auto decoded = decode(op1, immediate, op2);
				if (decoded.field == Field::flag_alias)
					continue;
				const auto instruction = encode(
					op1, immediate, op2);
				const auto result = build(instruction);
				const auto expected =
					expected_gadget(decoded, immediate);
				arm64_system_test::require(result.complete);
				arm64_system_test::require(
					result.program.words().front() ==
					address(expected));
				if (expected == uml_arm64_gadget_illegal) {
					arm64_system_test::require(
						result.program.instruction_count() ==
							1U);
					arm64_system_test::require(
						result.program.words().size() == 2U);
					arm64_system_test::require(
						result.program.words()[1] ==
							engine_test_code_base);
					continue;
				}
				const bool access_check =
					decoded.field ==
						Field::interrupt_mask_set ||
					decoded.field ==
						Field::interrupt_mask_clear;
				arm64_system_test::require(
					result.program.instruction_count() == 2U);
				arm64_system_test::require(
					result.program.words().size() ==
						(access_check ? 4U : 3U));
				std::size_t svc = 1U;
				if (access_check) {
					arm64_system_test::require(
						result.program.words()[1] ==
							engine_test_code_base);
					svc = 2U;
				}
				arm64_system_test::require(
					result.program.words()[svc] ==
						address(uml_arm64_gadget_svc));
				arm64_system_test::require(
					result.program.words()[svc + 1U] ==
						engine_test_code_base + 8U);
			}
}

void hot_path_shape()
{
	constexpr std::array boolean_fields{
		ssbs_gadgets, dit_gadgets, tco_gadgets,
		sm_gadgets, za_gadgets,
	};
	for (const auto &field : boolean_fields) {
		arm64_system_test::require(
			runtime_instruction_count(field[0]) == 3U);
		arm64_system_test::require(
			runtime_instruction_count(field[1]) == 4U);
		arm64_system_test::require(
			conditional_branch_count(field[0]) == 0U &&
			conditional_branch_count(field[1]) == 0U);
	}
	arm64_system_test::require(
		runtime_instruction_count(smza_gadgets[0]) == 4U);
	arm64_system_test::require(
		runtime_instruction_count(smza_gadgets[1]) == 5U);
	arm64_system_test::require(
		conditional_branch_count(smza_gadgets[0]) == 0U &&
		conditional_branch_count(smza_gadgets[1]) == 0U);

	for (unsigned immediate = 0; immediate < 16U; ++immediate)
		for (const auto gadget :
		     {daif_set_gadgets[immediate],
		      daif_clear_gadgets[immediate]}) {
			arm64_system_test::require(
				conditional_branch_count(gadget) == 1U);
			arm64_system_test::require(
				runtime_instruction_count(gadget) ==
					(immediate == 0U ? 5U : 9U));
		}
}

} // namespace
} // namespace uml::emulator::tests::base_pstate

int main()
{
	uml::emulator::tests::base_pstate::exhaustive_translation();
	uml::emulator::tests::base_pstate::hot_path_shape();
	return 0;
}
