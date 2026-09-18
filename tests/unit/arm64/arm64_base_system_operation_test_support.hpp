// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_base_feature_system_test_support.hpp"
#include "arm64_base_ic_test_support.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace arm64_base_system_operation_test {

namespace engine = uml::emulator::tests;
namespace feature_test = arm64_base_feature_system_test;
namespace sys_test = arm64_system_test;
using ::uml::cpu::ArmFeature;

enum class Requirement {
	baseline,
	dpb,
	dpb2,
	mec,
	mec_mte2,
	mte,
	mte2,
	mtetc,
	occmo,
	occmo_mte,
	pops,
	pops_mte2,
	rme,
	rme_mte2,
	tlbios,
	tlbirange,
	tlbiw,
	xs,
};

struct DcOperation final {
	const char *name;
	unsigned op1;
	unsigned crm;
	unsigned op2;
	Requirement requirement;
	bool accessible_at_el0;
};

/*
 * Complete 2026-06 A-profile DC operation table. Combined requirements are
 * retained explicitly instead of being inferred from the mnemonic.
 */
inline constexpr std::array dc_operations{
	DcOperation{"ivac", 0b000, 0b0110, 0b001,
		Requirement::baseline, false},
	DcOperation{"isw", 0b000, 0b0110, 0b010,
		Requirement::baseline, false},
	DcOperation{"igvac", 0b000, 0b0110, 0b011,
		Requirement::mte2, false},
	DcOperation{"igsw", 0b000, 0b0110, 0b100,
		Requirement::mte2, false},
	DcOperation{"igdvac", 0b000, 0b0110, 0b101,
		Requirement::mte2, false},
	DcOperation{"igdsw", 0b000, 0b0110, 0b110,
		Requirement::mte2, false},
	DcOperation{"csw", 0b000, 0b1010, 0b010,
		Requirement::baseline, false},
	DcOperation{"cgsw", 0b000, 0b1010, 0b100,
		Requirement::mte2, false},
	DcOperation{"cgdsw", 0b000, 0b1010, 0b110,
		Requirement::mte2, false},
	DcOperation{"cisw", 0b000, 0b1110, 0b010,
		Requirement::baseline, false},
	DcOperation{"cigsw", 0b000, 0b1110, 0b100,
		Requirement::mte2, false},
	DcOperation{"cigdsw", 0b000, 0b1110, 0b110,
		Requirement::mte2, false},
	DcOperation{"civaps", 0b000, 0b1111, 0b001,
		Requirement::pops, false},
	DcOperation{"cigdvaps", 0b000, 0b1111, 0b101,
		Requirement::pops_mte2, false},
	DcOperation{"zva", 0b011, 0b0100, 0b001,
		Requirement::baseline, true},
	DcOperation{"gva", 0b011, 0b0100, 0b011,
		Requirement::mte, false},
	DcOperation{"gzva", 0b011, 0b0100, 0b100,
		Requirement::mte, false},
	DcOperation{"zgbva", 0b011, 0b0100, 0b101,
		Requirement::mtetc, false},
	DcOperation{"gbva", 0b011, 0b0100, 0b111,
		Requirement::mtetc, false},
	DcOperation{"cvac", 0b011, 0b1010, 0b001,
		Requirement::baseline, true},
	DcOperation{"cgvac", 0b011, 0b1010, 0b011,
		Requirement::mte, false},
	DcOperation{"cgdvac", 0b011, 0b1010, 0b101,
		Requirement::mte, false},
	DcOperation{"cvaoc", 0b011, 0b1011, 0b000,
		Requirement::occmo, false},
	DcOperation{"cvau", 0b011, 0b1011, 0b001,
		Requirement::baseline, true},
	DcOperation{"cgdvaoc", 0b011, 0b1011, 0b111,
		Requirement::occmo_mte, false},
	DcOperation{"cvap", 0b011, 0b1100, 0b001,
		Requirement::dpb, false},
	DcOperation{"cgvap", 0b011, 0b1100, 0b011,
		Requirement::mte, false},
	DcOperation{"cgdvap", 0b011, 0b1100, 0b101,
		Requirement::mte, false},
	DcOperation{"cvadp", 0b011, 0b1101, 0b001,
		Requirement::dpb2, false},
	DcOperation{"cgvadp", 0b011, 0b1101, 0b011,
		Requirement::mte, false},
	DcOperation{"cgdvadp", 0b011, 0b1101, 0b101,
		Requirement::mte, false},
	DcOperation{"civac", 0b011, 0b1110, 0b001,
		Requirement::baseline, true},
	DcOperation{"cigvac", 0b011, 0b1110, 0b011,
		Requirement::mte, false},
	DcOperation{"cigdvac", 0b011, 0b1110, 0b101,
		Requirement::mte, false},
	DcOperation{"civaoc", 0b011, 0b1111, 0b000,
		Requirement::occmo, false},
	DcOperation{"cigdvaoc", 0b011, 0b1111, 0b111,
		Requirement::occmo_mte, false},
	DcOperation{"cipae", 0b100, 0b1110, 0b000,
		Requirement::mec, false},
	DcOperation{"cigdpae", 0b100, 0b1110, 0b111,
		Requirement::mec_mte2, false},
	DcOperation{"cipapa", 0b110, 0b1110, 0b001,
		Requirement::rme, false},
	DcOperation{"cigdpapa", 0b110, 0b1110, 0b101,
		Requirement::rme_mte2, false},
};
static_assert(dc_operations.size() == 40);

/* Complete scalar TLBI table, grouped by the feature column in the PDF. */
inline constexpr std::array<std::uint16_t, 16> tlbi_tlbios{{
	0x0408, 0x0409, 0x040a, 0x040b, 0x040d, 0x040f, 0x2408, 0x2409,
	0x240c, 0x240d, 0x240e, 0x2420, 0x2424, 0x3408, 0x3409, 0x340d,
}};
inline constexpr std::array<std::uint16_t, 30> tlbi_tlbirange{{
	0x0411, 0x0413, 0x0415, 0x0417, 0x0429, 0x042b, 0x042d, 0x042f,
	0x0431, 0x0433, 0x0435, 0x0437, 0x2402, 0x2406, 0x2411, 0x2415,
	0x2422, 0x2423, 0x2426, 0x2427, 0x2429, 0x242d, 0x2431, 0x2435,
	0x3411, 0x3415, 0x3429, 0x342d, 0x3431, 0x3435,
}};
inline constexpr std::array<std::uint16_t, 32> tlbi_baseline{{
	0x0418, 0x0419, 0x041a, 0x041b, 0x041d, 0x041f, 0x0438, 0x0439,
	0x043a, 0x043b, 0x043d, 0x043f, 0x2401, 0x2405, 0x2418, 0x2419,
	0x241c, 0x241d, 0x241e, 0x2421, 0x2425, 0x2438, 0x2439, 0x243c,
	0x243d, 0x243e, 0x3418, 0x3419, 0x341d, 0x3438, 0x3439, 0x343d,
}};
inline constexpr std::array<std::uint16_t, 78> tlbi_xs{{
	0x0488, 0x0489, 0x048a, 0x048b, 0x048d, 0x048f, 0x0491, 0x0493,
	0x0495, 0x0497, 0x0498, 0x0499, 0x049a, 0x049b, 0x049d, 0x049f,
	0x04a9, 0x04ab, 0x04ad, 0x04af, 0x04b1, 0x04b3, 0x04b5, 0x04b7,
	0x04b8, 0x04b9, 0x04ba, 0x04bb, 0x04bd, 0x04bf, 0x2481, 0x2482,
	0x2485, 0x2486, 0x2488, 0x2489, 0x248c, 0x248d, 0x248e, 0x2491,
	0x2495, 0x2498, 0x2499, 0x249c, 0x249d, 0x249e, 0x24a0, 0x24a1,
	0x24a2, 0x24a3, 0x24a4, 0x24a5, 0x24a6, 0x24a7, 0x24a9, 0x24ad,
	0x24b1, 0x24b5, 0x24b8, 0x24b9, 0x24bc, 0x24bd, 0x24be, 0x3488,
	0x3489, 0x348d, 0x3491, 0x3495, 0x3498, 0x3499, 0x349d, 0x34a9,
	0x34ad, 0x34b1, 0x34b5, 0x34b8, 0x34b9, 0x34bd,
}};
inline constexpr std::array<std::uint16_t, 6> tlbi_tlbiw{{
	0x2412, 0x242a, 0x2432, 0x2492, 0x24aa, 0x24b2,
}};
inline constexpr std::array<std::uint16_t, 4> tlbi_rme{{
	0x340c, 0x3423, 0x3427, 0x343c,
}};
static_assert(tlbi_tlbios.size() + tlbi_tlbirange.size() +
	tlbi_baseline.size() + tlbi_xs.size() + tlbi_tlbiw.size() +
	tlbi_rme.size() == 166);

[[nodiscard]] constexpr std::uint32_t system_register_pair(
	bool read, unsigned o0, unsigned op1, unsigned crn, unsigned crm,
	unsigned op2, unsigned rt) noexcept
{
	return 0xd550'0000U | (static_cast<unsigned>(read) << 21) |
		((o0 & 1U) << 19) | ((op1 & 7U) << 16) |
		((crn & 15U) << 12) | ((crm & 15U) << 8) |
		((op2 & 7U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t system_instruction_pair(
	unsigned op1, unsigned crn, unsigned crm, unsigned op2,
	unsigned rt) noexcept
{
	return 0xd548'0000U | ((op1 & 7U) << 16) |
		((crn & 15U) << 12) | ((crm & 15U) << 8) |
		((op2 & 7U) << 5) | (rt & 31U);
}

} // namespace arm64_base_system_operation_test
