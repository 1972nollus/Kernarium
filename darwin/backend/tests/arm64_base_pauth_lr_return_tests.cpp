// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

namespace test = arm64_pauth_test;

namespace {

[[nodiscard]] constexpr std::uint32_t return_immediate(
	bool key_b, unsigned immediate)
{
	return 0x5500'001fU | (static_cast<unsigned>(key_b) << 21) |
		((immediate & 0xffffU) << 5);
}

[[nodiscard]] constexpr std::uint32_t return_register(
	bool key_b, unsigned source)
{
	return 0xd65f'0be0U | (static_cast<unsigned>(key_b) << 10) |
		(source & 31U);
}

void immediate_forms_are_undefined()
{
	for (const bool key_b : {false, true})
		for (const unsigned immediate :
		     {0U, 1U, 0x7fffU, 0x8000U, 0xfffeU, 0xffffU})
			test::require_illegal(return_immediate(key_b, immediate));
}

void register_forms_are_undefined()
{
	for (const bool key_b : {false, true})
		for (unsigned source = 0; source < 31; ++source)
			test::require_illegal(return_register(key_b, source));
}

void register_31_remains_classic_retaa()
{
	/* Rm==11111 is RETAA/RETAB from base FEAT_PAuth, not an unavailable
	 * FEAT_PAuth_LR register form. Sign LR with SP and verify both keys. */
	for (const bool key_b : {false, true}) {
		const auto initial = test::state(
			0x0000'1234'5678'9abcULL, 30, 31,
			0x2468'ace0'1357'9bdfULL);
		const auto signed_result = test::execute_both(
			test::data_processing(key_b ? 1U : 0U, 30, 31), initial);
		const auto returned = test::execute_control_both(
			return_register(key_b, 31), signed_result.interpreted);
		test::require(returned.interpreted.pc ==
			test::stripped(initial.registers[30], false));
	}
}

} // namespace

int main()
{
	immediate_forms_are_undefined();
	register_forms_are_undefined();
	register_31_remains_classic_retaa();
	return 0;
}
