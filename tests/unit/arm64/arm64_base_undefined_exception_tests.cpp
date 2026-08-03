// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_exception_test_support.hpp"

#include <array>

namespace {

[[nodiscard]] constexpr std::string_view evidence_name(
	std::uint32_t instruction_base) noexcept
{
	switch (instruction_base) {
	case 0xd400'0002U: return "BASE-0178 HVC";
	case 0xd400'0003U: return "BASE-0378 SMC";
	case 0xd440'0000U: return "BASE-0177 HLT";
	case 0xd4a0'0001U: return "BASE-0146 DCPS1";
	case 0xd4a0'0002U: return "BASE-0147 DCPS2";
	case 0xd4a0'0003U: return "BASE-0148 DCPS3";
	default: return "base-el0-undefined-exception";
	}
}

void permanently_undefined()
{
	arm64_exception_test::Harness harness;
	for (unsigned immediate = 0; immediate <= 0xffffU; ++immediate)
		harness.check("base-permanently-undefined", immediate,
			uml::emulator::tests::ExpectedEvent::illegal_instruction,
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
			harness.check(evidence_name(instruction_base),
				arm64_exception_test::with_immediate(
					instruction_base, immediate),
				uml::emulator::tests::ExpectedEvent::
					illegal_instruction,
				false, true);

	for (const auto instruction : {
		0xd6bf'03e0U, // DRPS outside Debug state
		0xd69f'03e0U, // ERET at EL0
		0xd69f'0bffU, // ERETAA at EL0
		0xd69f'0fffU, // ERETAB at EL0
	})
		harness.check("base-el0-undefined-return", instruction,
			uml::emulator::tests::ExpectedEvent::illegal_instruction,
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
