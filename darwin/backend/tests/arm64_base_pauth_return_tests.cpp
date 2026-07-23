// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

namespace test = arm64_pauth_test;

namespace {

constexpr std::array values{
	0x0000'1234'5678'9abcULL,
	0xffff'9234'5678'9abcULL,
};

void return_forms()
{
	for (const bool key_b : {false, true})
		for (const auto value : values) {
			const auto initial = test::state(value, 30, 31,
				0x2468'ace0'1357'9bdfULL);
			const auto signed_result = test::execute_both(
				test::data_processing(key_b ? 1U : 0U, 30, 31),
				initial);
			const auto instruction = key_b ? 0xd65f'0fffU : 0xd65f'0bffU;
			const auto returned = test::execute_control_both(
				instruction, signed_result.interpreted);
			test::require(returned.interpreted.pc ==
				test::stripped(value, false));
			/* RETAA/RETAB authenticate a temporary and do not write LR. */
			test::require(returned.interpreted.registers ==
				signed_result.interpreted.registers);
		}
}

void production_specialization()
{
#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
	const auto a = test::gadgets::Builder{}.build(test::pc,
		std::array{0xd65f'0bffU});
	const auto b = test::gadgets::Builder{}.build(test::pc,
		std::array{0xd65f'0fffU});
	test::require(a.program.words().front() != b.program.words().front());
#endif
}

} // namespace

int main()
{
	return_forms();
	production_specialization();
	return 0;
}
