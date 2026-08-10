// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_system_test_support.hpp"

#include <array>
#include <cstdlib>
#include <optional>

namespace test = arm64_system_test;

namespace {

[[nodiscard]] constexpr bool supported(std::uint32_t instruction)
{
	const auto system = instruction & 0xffff'ffe0U;
	for (const auto value : std::array{
		0xd53b'd040U, // TPIDR_EL0 read
		0xd51b'd040U, // TPIDR_EL0 write
		0xd53b'd060U, // TPIDRRO_EL0 read
		0xd53b'00e0U, // DCZID_EL0
		0xd53b'0020U, // CTR_EL0
		0xd53b'4400U, // FPCR read
		0xd51b'4400U, // FPCR write
		0xd53b'4420U, // FPSR read
		0xd51b'4420U, // FPSR write
		0xd53b'4440U, // FPMR read
		0xd51b'4440U, // FPMR write
		0xd53b'4200U, // NZCV read
		0xd51b'4200U, // NZCV write
		0xd53b'e000U, // CNTFRQ_EL0
		0xd53b'e040U, // CNTVCT_EL0
		0xd53b'42a0U, // DIT read
		0xd51b'42a0U, // DIT write
	})
		if (system == value)
			return true;
	return false;
}

void complete_allocation(
	test::Harness &harness, std::optional<unsigned> selected_op1)
{
	/* MRS/MSR has one o0 bit and a complete 15-bit system-register selector.
	 * Check every selector and every transfer register. Unsupported registers
	 * must fail without observing or modifying any state in all three engines.
	 * The supported selectors are covered for every transfer register by the
	 * behavior suite. */
	for (const bool read : {false, true})
		for (unsigned o0 = 0; o0 < 2; ++o0)
			for (unsigned op1 = 0; op1 < 8; ++op1)
				if (!selected_op1 || *selected_op1 == op1)
				for (unsigned crn = 0; crn < 16; ++crn)
					for (unsigned crm = 0; crm < 16; ++crm)
						for (unsigned op2 = 0; op2 < 8; ++op2)
							for (unsigned transfer = 0; transfer < 32;
							     ++transfer) {
								const auto instruction = test::system_register(
									read, o0, op1, crn, crm, op2, transfer);
								if (!supported(instruction))
									harness.require_illegal(instruction);
							}
}

void named_inaccessible_registers(test::Harness &harness)
{
	/* CurrentEL is privileged. CNTPCT_EL0 is gated off while CNTVCT_EL0 is the
	 * profile's userspace counter. SME/RNG/context-number registers are absent,
	 * and control/ID registers remain privileged. */
	for (const auto instruction : {
		0xd538'4240U, // CurrentEL
		0xd53b'e020U, // CNTPCT_EL0
		test::system_register(true, 1, 3, 13, 0, 5, 0), // TPIDR2_EL0
		test::system_register(false, 1, 3, 13, 0, 5, 0),
		test::system_register(true, 1, 3, 2, 4, 0, 0), // RNDR
		test::system_register(true, 1, 3, 2, 4, 1, 0), // RNDRRS
		test::system_register(true, 1, 3, 13, 0, 7, 0), // SCXTNUM_EL0
		0xd538'1000U, // SCTLR_EL1
		test::system_register(true, 1, 0, 0, 6, 0, 0), // ID_AA64ISAR0_EL1
	})
		harness.require_illegal(instruction);
}

} // namespace

int main(int argc, char **argv)
{
	std::optional<unsigned> selected_op1;
	if (argc > 1) {
		const auto value = std::strtoul(argv[1], nullptr, 10);
		test::require(value < 8);
		selected_op1 = static_cast<unsigned>(value);
	}
	test::Harness harness;
	complete_allocation(harness, selected_op1);
	if (!selected_op1 || *selected_op1 == 0)
		named_inaccessible_registers(harness);
	harness.print_summary("arm64-base-system-register-allocation");
	return 0;
}
