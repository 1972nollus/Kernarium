// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_pauth_test_support.hpp"

namespace test = arm64_pauth_test;

namespace {

[[nodiscard]] constexpr std::uint32_t pacga(
	unsigned destination, unsigned source, unsigned modifier)
{
	return 0x9ac0'3000U | ((modifier & 31U) << 16) |
		((source & 31U) << 5) | (destination & 31U);
}

void every_register_encoding()
{
	for (unsigned destination = 0; destination < 32; ++destination)
		for (unsigned source = 0; source < 32; ++source)
			for (unsigned modifier = 0; modifier < 32; ++modifier) {
				auto initial = test::state(0xfeed'face'1234'5678ULL,
					destination, modifier,
					0x0123'4567'89ab'cdefULL);
				if (source < 31)
					initial.registers[source] =
						0x89ab'cdef'7654'3210ULL ^ source;
				/* Register overlap is intentional. The final architectural
				 * values are what both sources must observe before Rd is
				 * written. */
				const auto result = test::execute_both(
					pacga(destination, source, modifier), initial);
				test::require_only_destination_changed(
					initial, result.interpreted, destination);
				if (destination < 31)
					test::require((result.interpreted
						.registers[destination] & 0xffff'ffffULL) == 0);
			}
}

void production_specialization()
{
#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
	const auto registers = test::gadgets::Builder{}.build(test::pc,
		std::array{pacga(3, 7, 11)});
	const auto zero_source = test::gadgets::Builder{}.build(test::pc,
		std::array{pacga(3, 31, 11)});
	const auto stack_modifier = test::gadgets::Builder{}.build(test::pc,
		std::array{pacga(3, 7, 31)});
	test::require(registers.program.words().front() !=
		zero_source.program.words().front());
	test::require(registers.program.words().front() ==
		stack_modifier.program.words().front());
	const auto discard = test::gadgets::Builder{}.build(test::pc,
		std::array{pacga(31, 7, 11)});
	const auto nop = test::gadgets::Builder{}.build(test::pc,
		std::array{test::hint(0)});
	test::require(discard.program.words().front() == nop.program.words().front());
#endif
}

} // namespace

int main()
{
	every_register_encoding();
	production_specialization();
	return 0;
}
