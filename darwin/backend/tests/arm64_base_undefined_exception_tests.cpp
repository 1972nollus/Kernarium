// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_exception_test_support.hpp"

#include <array>

namespace {

void permanently_undefined()
{
	arm64_exception_test::Harness harness;
	for (unsigned immediate = 0; immediate <= 0xffffU; ++immediate)
		harness.check("base-permanently-undefined", immediate,
			uml::backend::tests::ExpectedEvent::illegal_instruction,
			false, true);
	harness.print_summary("arm64-base-permanently-undefined");
}

void unavailable_at_el0()
{
	arm64_exception_test::Harness harness;
	constexpr std::array bases{
		0xd400'0002U, // HVC
		0xd400'0003U, // SMC
		0xd440'0000U, // HLT outside Debug state
		0xd4a0'0001U, // DCPS1 outside Debug state
		0xd4a0'0002U, // DCPS2 outside Debug state
		0xd4a0'0003U, // DCPS3 outside Debug state
	};
	for (const auto instruction_base : bases)
		for (unsigned immediate = 0; immediate <= 0xffffU; ++immediate)
			harness.check("base-el0-undefined-exception",
				arm64_exception_test::with_immediate(
					instruction_base, immediate),
				uml::backend::tests::ExpectedEvent::
					illegal_instruction,
				false, true);

	for (const auto instruction : {
		0xd6bf'03e0U, // DRPS outside Debug state
		0xd69f'03e0U, // ERET at EL0
		0xd69f'0bffU, // ERETAA at EL0
		0xd69f'0fffU, // ERETAB at EL0
	})
		harness.check("base-el0-undefined-return", instruction,
			uml::backend::tests::ExpectedEvent::illegal_instruction,
			false, true);
	harness.print_summary("arm64-base-el0-undefined-exceptions");
}

} // namespace

int main(int argc, char **)
{
	if (argc == 1)
		permanently_undefined();
	else
		unavailable_at_el0();
	return 0;
}
