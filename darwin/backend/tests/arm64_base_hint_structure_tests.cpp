// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_hint_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>
#include <set>

namespace {

namespace gadgets = uml::backend::arm64::gadgets;
namespace engine_tests = uml::backend::tests;

extern "C" {
void uml_arm64_gadget_nop();
void uml_arm64_gadget_yield();
void uml_arm64_gadget_dgh();
void uml_arm64_gadget_csdb();
void uml_arm64_gadget_clrbhb();
void uml_arm64_gadget_illegal();
}

template<typename Function>
[[nodiscard]] std::uintptr_t address(Function function)
{
	return reinterpret_cast<std::uintptr_t>(function);
}

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction)
{
	return gadgets::Builder{}.build(
		engine_tests::engine_test_code_base,
		std::array{instruction});
}

[[nodiscard]] std::uintptr_t expected_primary_entry(unsigned immediate)
{
	switch (immediate) {
	case 1:
		return address(uml_arm64_gadget_yield);
	case 6:
		return address(uml_arm64_gadget_dgh);
	case 20:
		return address(uml_arm64_gadget_csdb);
	case 22:
		return address(uml_arm64_gadget_clrbhb);
	default:
		return address(uml_arm64_gadget_nop);
	}
}

struct BodyShape final {
	unsigned instructions{};
	unsigned conditional_branches{};
	bool terminal{};
};

[[nodiscard]] BodyShape body_shape(std::uintptr_t entry)
{
	const auto *code = reinterpret_cast<const std::uint32_t *>(entry);
	BodyShape result{};
	for (unsigned index = 0; index < 32U; ++index) {
		const auto instruction = code[index];
		if ((instruction & 0xffff'fc1fU) == 0xd61f'0000U) {
			result.terminal = true;
			break;
		}
		if ((instruction & 0xff00'0010U) == 0x5400'0000U ||
		    (instruction & 0x7e00'0000U) == 0x3400'0000U ||
		    (instruction & 0x7e00'0000U) == 0x3600'0000U)
			++result.conditional_branches;
		++result.instructions;
	}
	return result;
}

void primary_translation_and_dead_gadget_audit()
{
	std::set<std::uintptr_t> selected;
	for (unsigned immediate = 0; immediate < 128U; ++immediate) {
		if (!arm64_hint_test::is_owned_primary_hint(immediate))
			continue;
		const auto built =
			build(arm64_hint_test::primary_instruction(immediate));
		const auto expected = expected_primary_entry(immediate);
		arm64_hint_test::require(
			built.complete && !built.program.words().empty(),
			"primary HINT did not translate completely");
		arm64_hint_test::require(
			built.program.words().front() == expected,
			"primary HINT did not resolve its operation at translation");
		selected.insert(built.program.words().front());
	}

	const std::set<std::uintptr_t> expected{
		address(uml_arm64_gadget_nop),
		address(uml_arm64_gadget_yield),
		address(uml_arm64_gadget_dgh),
		address(uml_arm64_gadget_csdb),
		address(uml_arm64_gadget_clrbhb),
	};
	arm64_hint_test::require(selected == expected,
		"HINT gadget bank contains an unselected or missing entry");

	for (const auto entry : expected) {
		const auto shape = body_shape(entry);
		arm64_hint_test::require(shape.terminal,
			"HINT gadget did not terminate in the inspection window");
		arm64_hint_test::require(shape.conditional_branches == 0U,
			"HINT gadget retained a runtime conditional branch");
		arm64_hint_test::require(shape.instructions <= 2U,
			"HINT gadget retained work beyond one resolved operation");
	}
}

void gsb_profile_translation()
{
	for (unsigned operation = 0; operation < 2U; ++operation)
		for (const unsigned transfer : {0U, 31U}) {
			const auto gsb = build(
				arm64_hint_test::gsb_instruction(
					operation, transfer));
			arm64_hint_test::require(
				gsb.complete && !gsb.program.words().empty() &&
					gsb.program.words().front() ==
						address(uml_arm64_gadget_illegal),
				"GSB EL0 profile exception was not resolved in translation");
		}
}

} // namespace

int main()
{
	primary_translation_and_dead_gadget_audit();
	gsb_profile_translation();
	return 0;
}
