// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

namespace test = arm64_pauth_test;

namespace {

[[nodiscard]] constexpr std::uint32_t authenticate_sp_pc(
	bool key_b, unsigned immediate)
{
	return 0xf380'001fU | (static_cast<unsigned>(key_b) << 21) |
		((immediate & 0xffffU) << 5);
}

[[nodiscard]] constexpr std::uint32_t authenticate_sp_pc_register(
	bool key_b, unsigned source)
{
	return 0xdac1'901eU | (static_cast<unsigned>(key_b) << 10) |
		((source & 31U) << 5);
}

void fixed_undefined_forms()
{
	/* PACIA/PACIB/AUTIA/AUTIB 171615, PACIASPPC/PACIBSPPC, and
	 * PACNBIASPPC/PACNBIBSPPC. */
	constexpr std::array encodings{
		0xdac1'8bfeU, 0xdac1'8ffeU,
		0xdac1'bbfeU, 0xdac1'bffeU,
		0xdac1'a3feU, 0xdac1'a7feU,
		0xdac1'83feU, 0xdac1'87feU,
	};
	for (const auto instruction : encodings)
		test::require_illegal(instruction);
}

void immediate_authentication_forms()
{
	for (const bool key_b : {false, true})
		for (const unsigned immediate :
		     {0U, 1U, 0x7fffU, 0x8000U, 0xfffeU, 0xffffU})
			test::require_illegal(
				authenticate_sp_pc(key_b, immediate));
}

void register_authentication_forms()
{
	for (const bool key_b : {false, true})
		for (unsigned source = 0; source < 32; ++source)
			test::require_illegal(
				authenticate_sp_pc_register(key_b, source));
}

void pacm_feature_absent_hint()
{
	const auto initial = test::state(0x1234'5678'9abcULL, 17, 16);
	const auto result = test::execute_both(0xd503'24ffU, initial);
	test::require(result.interpreted.registers == initial.registers);
	test::require(result.interpreted.sp == initial.sp);
	test::require(result.interpreted.nzcv == initial.nzcv);
#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
	const auto pacm = test::gadgets::Builder{}.build(
		test::pc, std::array{0xd503'24ffU});
	const auto nop = test::gadgets::Builder{}.build(
		test::pc, std::array{test::hint(0)});
	test::require(pacm.program.words().front() == nop.program.words().front());
#endif
}

void production_undefined_specialization()
{
#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
	const auto unavailable = test::gadgets::Builder{}.build(
		test::pc, std::array{0xdac1'8bfeU});
	const auto reserved = test::gadgets::Builder{}.build(
		test::pc, std::array{authenticate_sp_pc_register(false, 3)});
	const auto illegal = test::gadgets::Builder{}.build(
		test::pc, std::array{0U});
	test::require(unavailable.program.words().front() ==
		illegal.program.words().front());
	test::require(reserved.program.words().front() ==
		illegal.program.words().front());
#endif
}

} // namespace

int main()
{
	fixed_undefined_forms();
	immediate_authentication_forms();
	register_authentication_forms();
	pacm_feature_absent_hint();
	production_undefined_specialization();
	return 0;
}
