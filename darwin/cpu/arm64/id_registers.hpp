/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "cpu/arm64/floating_controls.hpp"
#include "cpu/host_capabilities.hpp"

#include <array>
#include <cstdint>

namespace uml::cpu::arm64::detail {

/* EL0 reads of the AArch64 identification registers are UNDEFINED in hardware.
 * Linux catches that undefined instruction and completes the read with a
 * sanitized value instead (`try_emulate_mrs()` in
 * arch/arm64/kernel/cpufeature.c), so guest userspace executes them
 * unconditionally; AT_HWCAP advertises the facility as HWCAP_CPUID. Wine's
 * unix-side ntdll reads ID_AA64PFR0_EL1 during process start, which is why an
 * engine that reports these as illegal cannot load a Windows program at all.
 *
 * Linux emulates exactly Op0 == 3, Op1 == 0, CRn == 0, and CRm either 0 or in
 * [2, 7]. Everything else in the EL1 encoding space stays illegal here, as it
 * does on arm64.
 */
[[nodiscard]] inline constexpr bool emulated_id_register(
	std::uint32_t instruction) noexcept
{
	if ((instruction & 0xffff'f000U) != 0xd538'0000U)
		return false;
	const unsigned group = (instruction >> 8) & 15U;
	const unsigned selector = (instruction >> 5) & 7U;
	if (group == 0U)
		/* MIDR_EL1, MPIDR_EL1 and REVIDR_EL1 are the only registers Linux
		 * emulates in this group; the rest of it is unallocated. */
		return selector == 0U || selector == 5U || selector == 6U;
	return group >= 2U && group <= 7U;
}

/* This virtual CPU holds no JEP106 implementer code, part number or revision.
 * Architecture 0b1111 is the architectural encoding for "the identification
 * registers describe this implementation", which is exactly what a guest
 * reading MIDR_EL1 here must fall back on. The kernel, which cannot execute
 * this read, prints the same fields in /proc/cpuinfo from a copy of the value
 * (show_arm64_cpuinfo() in the shared kernel series), and the engine smoke
 * tests fail when the two disagree. */
inline constexpr std::uint64_t emulated_midr_el1 = 0x000f'0000ULL;

/* Linux reports the reserved-bit-only affinity rather than the caller's
 * topology (SYS_MPIDR_SAFE_VAL); keep the same value so guest userspace cannot
 * derive a vCPU identity from a read that carries none on arm64 either. */
inline constexpr std::uint64_t emulated_mpidr_el1 = 0x8000'0000ULL;

/* The registers whose fields carry features, by the MRS that reads each into
 * X0. */
inline constexpr std::uint32_t id_aa64pfr0_el1 = 0xd538'0400U;
inline constexpr std::uint32_t id_aa64isar0_el1 = 0xd538'0600U;
inline constexpr std::uint32_t id_aa64isar1_el1 = 0xd538'0620U;
inline constexpr std::uint32_t id_aa64isar2_el1 = 0xd538'0640U;
inline constexpr std::uint32_t id_aa64mmfr0_el1 = 0xd538'0700U;
inline constexpr std::uint32_t id_aa64mmfr1_el1 = 0xd538'0720U;
inline constexpr std::uint32_t id_aa64mmfr2_el1 = 0xd538'0740U;

/* These values are the virtual CPU's advertised feature set, and AT_HWCAP
 * and AT_HWCAP2 are read from them (`guest_hwcaps()` below). A field reports
 * a feature only where an engine has committed to executing it, on the host
 * predicate its native path uses, never the host's whole feature set: a guest
 * that selects an opcode from either answer must not reach one an engine
 * would treat as illegal. Fields Linux hides from userspace (the exception
 * levels, ID_AA64MMFR0_EL1 except ECV, ID_AA64MMFR1_EL1 except AFP, debug and
 * IMPLEMENTATION DEFINED registers) read as zero here for the same reason
 * they do on arm64: they describe state EL0 cannot act on.
 */
[[nodiscard]] inline std::uint64_t emulated_id_register_value(
	std::uint32_t instruction) noexcept
{
	const unsigned group = (instruction >> 8) & 15U;
	const unsigned selector = (instruction >> 5) & 7U;
	const auto &host = ::uml::cpu::host_capabilities();
	const auto field = [](std::uint64_t value, unsigned position) noexcept {
		return value << position;
	};
	if (group == 0U) {
		if (selector == 0U)
			return emulated_midr_el1;
		if (selector == 5U)
			return emulated_mpidr_el1;
		/* REVIDR_EL1 is IMPLEMENTATION DEFINED, and Linux emulates it
		 * with zero. */
		return 0;
	}
	if (group == 4U && selector == 0U) {
		/* ID_AA64PFR0_EL1. An FP or AdvSIMD field of zero already means
		 * "implemented"; one additionally means half precision, whose
		 * native scalar and vector paths both engines gate on this
		 * probe. DIT: PSTATE.DIT is settable and readable. */
		std::uint64_t value = 0;
		if (host.fp16)
			value |= field(1, 16) | field(1, 20);
		if (host.has(ArmFeature::dit))
			value |= field(1, 48);
		return value;
	}
	if (group == 6U && selector == 0U) {
		/* ID_AA64ISAR0_EL1. The crypto, CRC32, LSE, RDM, dot-product
		 * and FHM families run as the host's own instructions. TS is
		 * CFINV, RMIF and SETF, then AXFLAG and XAFLAG. */
		std::uint64_t value = 0;
		if (host.aes)
			value |= field(host.pmull ? 2 : 1, 4);
		if (host.sha1)
			value |= field(1, 8);
		if (host.sha2 || host.sha512)
			value |= field(host.sha512 ? 2 : 1, 12);
		if (host.crc32)
			value |= field(1, 16);
		if (host.has(ArmFeature::lse))
			value |= field(2, 20);
		if (host.has(ArmFeature::rdm))
			value |= field(1, 28);
		if (host.sha3)
			value |= field(1, 32);
		if (host.has(ArmFeature::dotprod))
			value |= field(1, 44);
		if (host.has(ArmFeature::fhm))
			value |= field(1, 48);
		if (host.has(ArmFeature::flag_m))
			value |= field(
				host.has(ArmFeature::flag_m2) ? 2 : 1, 52);
		return value;
	}
	if (group == 6U && selector == 1U) {
		/* ID_AA64ISAR1_EL1. Both engines correct the Z flag FJCVTZS
		 * sets on some cores, such as the M1, for a subnormal FPCR.FZ
		 * flushes, so JSCVT follows the host. The gadget bank has a
		 * native RCpc load and store for every size. DPB stays zero
		 * because no engine implements DC CVAP, and the
		 * pointer-authentication fields because the virtual CPU has no
		 * FEAT_PAuth. */
		std::uint64_t value = 0;
		if (host.has(ArmFeature::jscvt))
			value |= field(1, 12);
		if (host.has(ArmFeature::fcma))
			value |= field(1, 16);
		if (host.has(ArmFeature::lrcpc))
			value |= field(
				host.has(ArmFeature::lrcpc2) ? 2 : 1, 20);
		if (host.has(ArmFeature::frintts))
			value |= field(1, 32);
		if (host.has(ArmFeature::sb))
			value |= field(1, 36);
		if (host.has(ArmFeature::bf16))
			value |= field(1, 44);
		if (host.has(ArmFeature::i8mm))
			value |= field(1, 52);
		return value;
	}
	if (group == 6U && selector == 2U)
		/* ID_AA64ISAR2_EL1. Both engines execute FEAT_MOPS, one host
		 * memmove or memset per page region, and FEAT_CSSC, natively or
		 * as base sequences, on every host; RPRES, the 12-bit estimates
		 * under FPCR.AH, follows the host. */
		return field(1, 16) | field(1, 52) |
			(implements_rpres(host) ? field(1, 4) : 0);
	if (group == 7U && selector == 0U)
		/* ID_AA64MMFR0_EL1. ECV, the one field Linux shows EL0: both
		 * engines read CNTVCTSS_EL0 with the host's instruction. */
		return host.has(ArmFeature::ecv) ? field(1, 60) : 0;
	if (group == 7U && selector == 1U)
		/* ID_AA64MMFR1_EL1. AFP, the one field Linux shows EL0, on the
		 * predicate that names the host features exact FIZ, AH and
		 * NEP take. */
		return implements_afp(host) ? field(1, 44) : 0;
	if (group == 7U && selector == 2U)
		/* ID_AA64MMFR2_EL1.AT, FEAT_LSE2: an LSE atomic on an address
		 * that is not naturally aligned is single-copy atomic rather
		 * than a fault. */
		return host.has(ArmFeature::lse2) ? field(1, 32) : 0;
	/* Every remaining register in the emulated space describes an AArch32
	 * profile this AArch64-only guest does not implement, an extension no
	 * engine executes, or state Linux keeps hidden from EL0. */
	return 0;
}

/* A bit of AT_HWCAP or AT_HWCAP2 and the field it is read from: bit
 * `hwcap`, numbered as the kernel's KERNEL_HWCAP_* (AT_HWCAP's as 0-63,
 * AT_HWCAP2's as 64-127), is set where the 4-bit field at `shift` holds at
 * least `minimum`. */
struct HwcapField final {
	std::uint32_t id_register;
	unsigned shift;
	unsigned minimum;
	unsigned hwcap;
};

/* arm64 Linux's own derivation (arm64_elf_hwcaps in
 * arch/arm64/kernel/cpufeature.c), restricted to the fields the values
 * above can set. */
inline constexpr HwcapField hwcap_fields[]{
	{id_aa64pfr0_el1, 16, 1, 9},		/* FP: FPHP */
	{id_aa64pfr0_el1, 20, 1, 10},		/* AdvSIMD: ASIMDHP */
	{id_aa64pfr0_el1, 48, 1, 24},		/* DIT */
	{id_aa64isar0_el1, 4, 1, 3},		/* AES */
	{id_aa64isar0_el1, 4, 2, 4},		/* AES: PMULL */
	{id_aa64isar0_el1, 8, 1, 5},		/* SHA1 */
	{id_aa64isar0_el1, 12, 1, 6},		/* SHA2 */
	{id_aa64isar0_el1, 12, 2, 21},		/* SHA2: SHA512 */
	{id_aa64isar0_el1, 16, 1, 7},		/* CRC32 */
	{id_aa64isar0_el1, 20, 2, 8},		/* Atomic: ATOMICS */
	{id_aa64isar0_el1, 28, 1, 12},		/* RDM: ASIMDRDM */
	{id_aa64isar0_el1, 32, 1, 17},		/* SHA3 */
	{id_aa64isar0_el1, 44, 1, 20},		/* DP: ASIMDDP */
	{id_aa64isar0_el1, 48, 1, 23},		/* FHM: ASIMDFHM */
	{id_aa64isar0_el1, 52, 1, 27},		/* TS: FLAGM */
	{id_aa64isar0_el1, 52, 2, 64 + 7},	/* TS: FLAGM2 */
	{id_aa64isar1_el1, 12, 1, 13},		/* JSCVT */
	{id_aa64isar1_el1, 16, 1, 14},		/* FCMA */
	{id_aa64isar1_el1, 20, 1, 15},		/* LRCPC */
	{id_aa64isar1_el1, 20, 2, 26},		/* LRCPC: ILRCPC */
	{id_aa64isar1_el1, 32, 1, 64 + 8},	/* FRINTTS: FRINT */
	{id_aa64isar1_el1, 36, 1, 29},		/* SB */
	{id_aa64isar1_el1, 44, 1, 64 + 14},	/* BF16 */
	{id_aa64isar1_el1, 52, 1, 64 + 13},	/* I8MM */
	{id_aa64isar2_el1, 4, 1, 64 + 21},	/* RPRES */
	{id_aa64isar2_el1, 16, 1, 64 + 43},	/* MOPS */
	{id_aa64isar2_el1, 52, 1, 64 + 34},	/* CSSC */
	{id_aa64mmfr0_el1, 60, 1, 64 + 19},	/* ECV */
	{id_aa64mmfr1_el1, 44, 1, 64 + 20},	/* AFP */
	{id_aa64mmfr2_el1, 32, 1, 25},		/* AT: USCAT */
};

/* AT_HWCAP and AT_HWCAP2 for a host, which `darwin_uml_guest_hwcap()` and
 * `darwin_uml_guest_hwcap2()` return to the guest kernel. Each word is read
 * from the identification registers exactly as arm64 Linux reads it, so a
 * guest gets the same answer from MRS and from its auxiliary vector. The
 * registers are read in a scope for `host`, because the kernel asks on a
 * thread where no engine need have installed one. */
[[nodiscard]] inline std::array<unsigned long, 2> guest_hwcaps(
	const HostCapabilities &host) noexcept
{
	/* FP and ASIMD, whose fields are never 0b1111 here; EVTSTRM, the
	 * promise that WFE wakes once per event-stream period, which holds
	 * because both engines complete WFE at once; and CPUID, the emulation
	 * of these reads itself, which Linux also sets unconditionally. */
	std::array<unsigned long, 2> words{
		(1UL << 0) | (1UL << 1) | (1UL << 2) | (1UL << 11), 0};
	const HostCapabilitiesScope scope{host};
	for (const auto &bit : hwcap_fields)
		if (((emulated_id_register_value(bit.id_register) >>
		      bit.shift) & 15U) >= bit.minimum)
			words[bit.hwcap / 64U] |= 1UL << (bit.hwcap % 64U);
	return words;
}

} // namespace uml::cpu::arm64::detail
