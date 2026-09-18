// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_hint_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>

namespace {

namespace engine_tests = uml::emulator::tests;
namespace gadgets = uml::cpu::arm64::gadgets;

void production_specialization()
{
	const auto pc = engine_tests::engine_test_code_base;
	const auto nop = gadgets::Builder{}.build(
		pc, std::array{arm64_hint_test::primary_instruction(0)});
	for (const unsigned immediate : {
		7U, 8U, 10U, 12U, 14U, 24U, 25U,
		26U, 27U, 28U, 29U, 30U, 31U,
	}) {
		const auto specialized = gadgets::Builder{}.build(
			pc, std::array{
				arm64_hint_test::primary_instruction(immediate)});
		arm64_system_test::require(
			specialized.program.words().front() !=
			nop.program.words().front());
	}
	const auto profile_absent = gadgets::Builder{}.build(
		pc, std::array{arm64_hint_test::primary_instruction(39)});
	arm64_system_test::require(
		profile_absent.program.words().front() ==
		nop.program.words().front());
}

} // namespace

int main()
{
	production_specialization();
	return 0;
}
