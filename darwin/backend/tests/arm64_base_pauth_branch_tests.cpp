// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

namespace test = arm64_pauth_test;

namespace {

constexpr std::array values{
	0x0000'1234'5678'9abcULL,
	0xffff'9234'5678'9abcULL,
};
constexpr std::array modifiers{0U, 7U, 16U, 30U, 31U};

[[nodiscard]] constexpr std::uint32_t authenticated_branch(
	bool link, bool key_b, bool zero_modifier,
	unsigned target, unsigned modifier = 31)
{
	return (zero_modifier ? 0xd61f'0800U : 0xd71f'0800U) |
		(static_cast<unsigned>(link) << 21) |
		(static_cast<unsigned>(key_b) << 10) |
		((target & 31U) << 5) |
		(modifier & 31U);
}

void register_modifier_forms()
{
	for (const bool link : {false, true})
		for (const bool key_b : {false, true})
			for (unsigned target = 0; target < 31; ++target)
				for (const unsigned modifier : modifiers) {
					if (modifier == target)
						continue;
					for (const auto value : values) {
						const auto initial = test::state(value, target,
							modifier, 0x2468'ace0'1357'9bdfULL);
						const auto signed_result = test::execute_both(
							test::data_processing(key_b ? 1U : 0U,
								target, modifier), initial);
						const auto branched = test::execute_control_both(
							authenticated_branch(link, key_b, false,
								target, modifier),
							signed_result.interpreted);
						test::require(branched.interpreted.pc ==
							test::stripped(initial.registers[target], false));
						if (link)
							test::require(branched.interpreted.registers[30] ==
								test::pc + 4);
						else
							test::require(branched.interpreted.registers ==
								signed_result.interpreted.registers);
					}
				}
}

void zero_modifier_forms()
{
	for (const bool link : {false, true})
		for (const bool key_b : {false, true})
			for (unsigned target = 0; target < 31; ++target)
				for (const auto value : values) {
					const auto initial = test::state(value, target);
					const auto signed_result = test::execute_both(
						test::data_processing(key_b ? 9U : 8U, target),
						initial);
					const auto branched = test::execute_control_both(
						authenticated_branch(link, key_b, true, target),
						signed_result.interpreted);
					test::require(branched.interpreted.pc ==
						test::stripped(initial.registers[target], false));
					if (link)
						test::require(branched.interpreted.registers[30] ==
							test::pc + 4);
				}
}

void xzr_target_forms()
{
	for (const bool link : {false, true})
		for (const bool key_b : {false, true})
			for (const bool zero_modifier : {false, true}) {
				const auto initial = test::state(0, 31, 7,
					0x2468'ace0'1357'9bdfULL);
				const auto result = test::execute_control_both(
					authenticated_branch(link, key_b, zero_modifier,
						31, zero_modifier ? 31U : 7U), initial);
				if (link)
					test::require(result.interpreted.registers[30] ==
						test::pc + 4);
			}
}

void reserved_zero_modifier_encodings()
{
	for (const bool link : {false, true})
		for (const bool key_b : {false, true})
			for (unsigned modifier = 0; modifier < 31; ++modifier)
				for (const unsigned target : {0U, 17U, 31U})
					test::require_illegal(authenticated_branch(
						link, key_b, true, target, modifier));
}

void production_specialization()
{
#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
	const auto braa = test::gadgets::Builder{}.build(test::pc,
		std::array{authenticated_branch(false, false, false, 3, 7)});
	const auto braaz = test::gadgets::Builder{}.build(test::pc,
		std::array{authenticated_branch(false, false, true, 3)});
	const auto brab = test::gadgets::Builder{}.build(test::pc,
		std::array{authenticated_branch(false, true, false, 3, 7)});
	const auto blraa = test::gadgets::Builder{}.build(test::pc,
		std::array{authenticated_branch(true, false, false, 3, 7)});
	const auto target_zero = test::gadgets::Builder{}.build(test::pc,
		std::array{authenticated_branch(false, false, false, 31, 7)});
	test::require(braa.program.words().front() != braaz.program.words().front());
	test::require(braa.program.words().front() != brab.program.words().front());
	test::require(braa.program.words().front() != blraa.program.words().front());
	test::require(braa.program.words().front() !=
		target_zero.program.words().front());
#endif
}

} // namespace

int main()
{
	register_modifier_forms();
	zero_modifier_forms();
	xzr_target_forms();
	reserved_zero_modifier_encodings();
	production_specialization();
	return 0;
}
