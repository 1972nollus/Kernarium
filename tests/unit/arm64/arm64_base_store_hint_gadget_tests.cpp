// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_base_store_hint_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

using uml::testing::a64::conditional_branch;
using uml::testing::a64::indirect_branch;

namespace store_hint = arm64_base_store_hint_test;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace engine = uml::emulator::tests;

extern "C" void uml_arm64_gadget_nop();

namespace {

void all_forms_fold_to_nop()
{
	const auto nop = reinterpret_cast<std::uintptr_t>(
		uml_arm64_gadget_nop);
	const auto nop_program = gadgets::Builder{}.build(
		engine::engine_test_code_base,
		std::array{arm64_hint_test::primary_instruction(0)});
	arm64_hint_test::require(nop_program.complete,
		"common NOP did not translate completely");
	for (const auto &variant : store_hint::variants) {
		const auto built = gadgets::Builder{}.build(
			engine::engine_test_code_base,
			std::array{
				arm64_hint_test::primary_instruction(
					variant.immediate)});
		arm64_hint_test::require(
			built.complete && built.program.instruction_count() == 1U,
			"store hint did not translate completely");
		arm64_hint_test::require(
			built.program.words().size() ==
				nop_program.program.words().size() &&
				std::equal(built.program.words().begin(),
					built.program.words().end(),
					nop_program.program.words().begin()) &&
				built.program.words().front() == nop,
			"store hint retained runtime decode or a dedicated gadget");
	}

	const auto *body = reinterpret_cast<const std::uint32_t *>(nop);
	unsigned instructions = 0;
	for (; instructions < 4U; ++instructions) {
		const auto instruction = body[instructions];
		if (indirect_branch(instruction))
			break;
		arm64_hint_test::require(!conditional_branch(instruction),
			"NOP gadget contains a conditional branch");
	}
	arm64_hint_test::require(instructions <= 1U,
		"NOP gadget contains unexpected runtime work");
}

} // namespace

int main()
{
	all_forms_fold_to_nop();
}
