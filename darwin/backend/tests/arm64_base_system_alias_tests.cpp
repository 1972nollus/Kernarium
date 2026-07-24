// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_system_test_support.hpp"

#include <array>

namespace test = arm64_system_test;

namespace {

void illegal_all_transfers(test::Harness &harness, bool result,
	unsigned op1, unsigned crn, unsigned crm, unsigned op2)
{
	for (unsigned transfer = 0; transfer < 32; ++transfer)
		harness.require_illegal(test::system_instruction(result, op1, crn,
			crm, op2, transfer));
}

void fixed_aliases()
{
	test::Harness harness;
	/* APAS; CFP, DVP, COSP, CPP; and TRCIT.  Their extensions are not
	 * advertised by the static EL0 profile. */
	for (const auto fields : {
		std::array<unsigned, 4>{6, 7, 0, 0},
		std::array<unsigned, 4>{3, 7, 3, 4},
		std::array<unsigned, 4>{3, 7, 3, 5},
		std::array<unsigned, 4>{3, 7, 3, 6},
		std::array<unsigned, 4>{3, 7, 3, 7},
		std::array<unsigned, 4>{3, 7, 2, 7}})
		illegal_all_transfers(harness, false, fields[0], fields[1],
			fields[2], fields[3]);

	/* Both BRB operations: IALL and INJ. */
	for (const unsigned operation : {4U, 5U})
		illegal_all_transfers(harness, false, 1, 7, 2, operation);

	/* Both GSB operations: SYS and ACK. */
	for (const unsigned operation : {0U, 1U})
		illegal_all_transfers(harness, false, 0, 12, 0, operation);
}

void address_translation_versions()
{
	test::Harness harness;
	/* Every AT operation listed by the 2026-06 profile, including the PAN2 and
	 * ATS1A variants. Address translation is privileged in this EL0 VM. */
	constexpr std::array operations{
		std::array<unsigned, 3>{0, 8, 0}, // S1E1R
		std::array<unsigned, 3>{0, 8, 1}, // S1E1W
		std::array<unsigned, 3>{0, 8, 2}, // S1E0R
		std::array<unsigned, 3>{0, 8, 3}, // S1E0W
		std::array<unsigned, 3>{0, 9, 0}, // S1E1RP
		std::array<unsigned, 3>{0, 9, 1}, // S1E1WP
		std::array<unsigned, 3>{0, 9, 2}, // S1E1A
		std::array<unsigned, 3>{4, 8, 0}, // S1E2R
		std::array<unsigned, 3>{4, 8, 1}, // S1E2W
		std::array<unsigned, 3>{4, 8, 4}, // S12E1R
		std::array<unsigned, 3>{4, 8, 5}, // S12E1W
		std::array<unsigned, 3>{4, 8, 6}, // S12E0R
		std::array<unsigned, 3>{4, 8, 7}, // S12E0W
		std::array<unsigned, 3>{4, 9, 2}, // S1E2A
		std::array<unsigned, 3>{6, 8, 0}, // S1E3R
		std::array<unsigned, 3>{6, 8, 1}, // S1E3W
		std::array<unsigned, 3>{6, 9, 2}, // S1E3A
	};
	for (const auto fields : operations)
		for (const unsigned transfer : {0U, 7U, 30U, 31U})
			harness.require_illegal(test::system_instruction(false,
				fields[0], 7, fields[1], fields[2], transfer));
}

void cache_instruction_aliases()
{
	test::Harness harness;
	/* Exhaust the shared AT/BRB/DC/IC/prediction/GCS/TRCIT C7 namespace.
	 * DC ZVA, DC CVAU, and IC IVAU are the only enabled EL0 operations. This
	 * covers every other DC and IC operation version, including optional-tag
	 * forms. */
	for (unsigned op1 = 0; op1 < 8; ++op1)
		for (unsigned crm = 0; crm < 16; ++crm)
			for (unsigned op2 = 0; op2 < 8; ++op2)
				for (const unsigned transfer : {0U, 31U}) {
					const auto instruction = test::system_instruction(false,
						op1, 7, crm, op2, transfer);
					const auto operation = instruction & 0xffff'ffe0U;
					if (operation != 0xd50b'7420U &&
					    operation != 0xd50b'7b20U &&
					    operation != 0xd50b'7520U)
						harness.require_illegal(instruction);
				}
}

void generic_interrupt_controller_versions()
{
	test::Harness harness;
	/* Cover the complete GIC alias field, including all currently named
	 * physical, virtual, and local operations and its reserved tuples. */
	for (unsigned op1 = 0; op1 < 8; ++op1)
		for (const unsigned crm : {1U, 2U})
			for (unsigned op2 = 0; op2 < 8; ++op2)
				for (const unsigned transfer : {0U, 31U})
					harness.require_illegal(test::system_instruction(false,
						op1, 12, crm, op2, transfer));

	/* GICR CDIA/CDNMIA are SYSL result forms. */
	for (const unsigned op2 : {0U, 1U})
		illegal_all_transfers(harness, true, 0, 12, 3, op2);
}

void tlbi_versions()
{
	test::Harness harness;
	/* CRn C8/C9 is the full scalar TLBI namespace, including range, OS, IS,
	 * NXS, EL1/EL2/EL3, Realm, and optional-operand versions. */
	for (unsigned crn = 8; crn <= 9; ++crn)
		for (unsigned op1 = 0; op1 < 8; ++op1)
			for (unsigned crm = 0; crm < 16; ++crm)
				for (unsigned op2 = 0; op2 < 8; ++op2)
					for (const unsigned transfer : {0U, 31U})
						harness.require_illegal(test::system_instruction(false,
							op1, crn, crm, op2, transfer));
}

} // namespace

int main()
{
	fixed_aliases();
	address_translation_versions();
	cache_instruction_aliases();
	generic_interrupt_controller_versions();
	tlbi_versions();
	return 0;
}
