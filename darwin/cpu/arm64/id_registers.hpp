/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "cpu/host_capabilities.hpp"

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

/* The values below carry exactly the features `darwin_uml_guest_hwcap()`
 * advertises, on the identical host predicates, and nothing else. That is the
 * same contract AT_HWCAP already follows: a guest may select an opcode only
 * where an engine has committed to executing it, and an identification
 * register is just the second way to ask. Fields Linux hides from userspace
 * (the exception levels, the whole of ID_AA64MMFR0_EL1 except ECV, debug and
 * IMPLEMENTATION DEFINED registers) read as zero here for the same reason they
 * do on arm64: they describe state EL0 cannot act on.
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
		 * "implemented"; one additionally means half precision. */
		std::uint64_t value = 0;
		if (host.fp16)
			value |= field(1, 16) | field(1, 20);
		if (host.has(ArmFeature::dit))
			value |= field(1, 48);
		return value;
	}
	if (group == 6U && selector == 0U) {
		/* ID_AA64ISAR0_EL1. */
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
		/* ID_AA64ISAR1_EL1. DPB stays zero because no engine implements
		 * DC CVAP, and the pointer-authentication fields stay zero
		 * because AT_HWCAP does not advertise PACA/PACG either. */
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
	if (group == 7U && selector == 2U)
		/* ID_AA64MMFR2_EL1.AT is where Linux derives HWCAP_USCAT, the
		 * single-copy atomicity guarantee for an unaligned LSE atomic. */
		return host.has(ArmFeature::lse2) ? field(1, 32) : 0;
	/* Every remaining register in the emulated space describes an AArch32
	 * profile this AArch64-only guest does not implement, an extension no
	 * engine executes, or state Linux keeps hidden from EL0. */
	return 0;
}

} // namespace uml::cpu::arm64::detail
