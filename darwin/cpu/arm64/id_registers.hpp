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

/* The values below carry exactly the features `darwin_uml_guest_hwcap()`
 * advertises, on the identical host predicates, and nothing else. That is the
 * same contract AT_HWCAP already follows: a guest may select an opcode only
 * where an engine has committed to executing it, and an identification
 * register is just the second way to ask. Fields Linux hides from userspace
 * (the exception levels, ID_AA64MMFR0_EL1 except ECV, ID_AA64MMFR1_EL1
 * except AFP, debug and IMPLEMENTATION DEFINED registers) read as zero here
 * for the same reason they do on arm64: they describe state EL0 cannot act
 * on.
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
	if (group == 6U && selector == 2U)
		/* ID_AA64ISAR2_EL1. Both engines execute FEAT_MOPS and
		 * FEAT_CSSC on every host; RPRES follows the host. */
		return field(1, 16) | field(1, 52) |
			(implements_rpres(host) ? field(1, 4) : 0);
	if (group == 7U && selector == 0U)
		/* ID_AA64MMFR0_EL1. ECV is the one field Linux shows EL0, and
		 * where it derives HWCAP2_ECV. */
		return host.has(ArmFeature::ecv) ? field(1, 60) : 0;
	if (group == 7U && selector == 1U)
		/* ID_AA64MMFR1_EL1.AFP is the one field of it Linux shows EL0,
		 * and where it derives HWCAP2_AFP. */
		return implements_afp(host) ? field(1, 44) : 0;
	if (group == 7U && selector == 2U)
		/* ID_AA64MMFR2_EL1.AT is where Linux derives HWCAP_USCAT, the
		 * single-copy atomicity guarantee for an unaligned LSE atomic. */
		return host.has(ArmFeature::lse2) ? field(1, 32) : 0;
	/* Every remaining register in the emulated space describes an AArch32
	 * profile this AArch64-only guest does not implement, an extension no
	 * engine executes, or state Linux keeps hidden from EL0. */
	return 0;
}

/* Keep synchronized with linux/arch/arm64/um/asm/elf.h and the
 * gadget/JIT instruction profile.  In particular, do not forward the
 * host's complete feature set: doing so would let libc generate instructions
 * that no guest engine has promised to implement.
 */
inline constexpr unsigned long hwcap_fp = 1UL << 0;
inline constexpr unsigned long hwcap_asimd = 1UL << 1;
inline constexpr unsigned long hwcap_evtstrm = 1UL << 2;
inline constexpr unsigned long hwcap_aes = 1UL << 3;
inline constexpr unsigned long hwcap_pmull = 1UL << 4;
inline constexpr unsigned long hwcap_sha1 = 1UL << 5;
inline constexpr unsigned long hwcap_sha2 = 1UL << 6;
inline constexpr unsigned long hwcap_crc32 = 1UL << 7;
inline constexpr unsigned long hwcap_atomics = 1UL << 8;
inline constexpr unsigned long hwcap_fphp = 1UL << 9;
inline constexpr unsigned long hwcap_asimdhp = 1UL << 10;
inline constexpr unsigned long hwcap_cpuid = 1UL << 11;
inline constexpr unsigned long hwcap_asimdrdm = 1UL << 12;
inline constexpr unsigned long hwcap_jscvt = 1UL << 13;
inline constexpr unsigned long hwcap_fcma = 1UL << 14;
inline constexpr unsigned long hwcap_lrcpc = 1UL << 15;
inline constexpr unsigned long hwcap_sha3 = 1UL << 17;
inline constexpr unsigned long hwcap_asimddp = 1UL << 20;
inline constexpr unsigned long hwcap_sha512 = 1UL << 21;
inline constexpr unsigned long hwcap_asimdfhm = 1UL << 23;
inline constexpr unsigned long hwcap_dit = 1UL << 24;
inline constexpr unsigned long hwcap_uscat = 1UL << 25;
inline constexpr unsigned long hwcap_ilrcpc = 1UL << 26;
inline constexpr unsigned long hwcap_flagm = 1UL << 27;
inline constexpr unsigned long hwcap_sb = 1UL << 29;
inline constexpr unsigned long hwcap2_flagm2 = 1UL << 7;
inline constexpr unsigned long hwcap2_frint = 1UL << 8;
inline constexpr unsigned long hwcap2_i8mm = 1UL << 13;
inline constexpr unsigned long hwcap2_bf16 = 1UL << 14;
inline constexpr unsigned long hwcap2_ecv = 1UL << 19;
inline constexpr unsigned long hwcap2_afp = 1UL << 20;
inline constexpr unsigned long hwcap2_rpres = 1UL << 21;
inline constexpr unsigned long hwcap2_cssc = 1UL << 34;
inline constexpr unsigned long hwcap2_mops = 1UL << 43;

/* AT_HWCAP and AT_HWCAP2 for this host, which `darwin_uml_guest_hwcap()` and
 * `darwin_uml_guest_hwcap2()` return to the guest kernel.
 *
 * Production gadget programs use native host crypto operations.  Expose
 * only extensions that are both implemented by the virtual CPU and safe
 * on this host; never let libc select an opcode that would SIGILL inside
 * the gadget or JIT engine.
 */
[[nodiscard]] inline std::array<unsigned long, 2> guest_hwcaps(
	const HostCapabilities &host) noexcept
{
	/* EL0 reads of the AArch64 identification registers, which every engine
	 * emulates from `cpu/arm64/id_registers.hpp` exactly as arm64 Linux does
	 * from its undefined-instruction hook. Linux advertises this
	 * unconditionally in `setup_elf_hwcaps()`, and the values those reads
	 * return here are built from the same host predicates as the feature bits
	 * below, so both ways of asking give a guest the same answer. */
	unsigned long hwcap = hwcap_fp | hwcap_asimd | hwcap_cpuid;
	/* The timer event stream. arm64 Linux advertises it whenever its
	 * architected timer generates one, as a promise that WFE wakes at least
	 * once per period (about 100 us) without an SEV. Both engines complete
	 * WFE at once, so the promise holds on every host. */
	hwcap |= hwcap_evtstrm;
	if (host.aes)
		hwcap |= hwcap_aes;
	if (host.pmull)
		hwcap |= hwcap_pmull;
	if (host.sha1)
		hwcap |= hwcap_sha1;
	if (host.sha2)
		hwcap |= hwcap_sha2;
	if (host.crc32)
		hwcap |= hwcap_crc32;
	if (host.has(ArmFeature::lse))
		hwcap |= hwcap_atomics;
	if (host.sha3)
		hwcap |= hwcap_sha3;
	if (host.sha512)
		hwcap |= hwcap_sha512;
	/* SDOT/UDOT/USDOT. Both engines execute this family: the gadget bank
	 * selects native dot gadgets behind `ArmFeature::dotprod`
	 * (gadget_memory_selection.cpp) and the JIT emits it natively behind the
	 * same predicate (JitCompilerFamilies::emit_simd_dot). Gating the
	 * advertisement on the identical host predicate those native paths use
	 * keeps the promise exact: guest libc and guest JITs may select these
	 * opcodes only where an engine has committed to executing them.
	 */
	if (host.has(ArmFeature::dotprod))
		hwcap |= hwcap_asimddp;
	/* Half precision. Both engines gate their native 16-bit scalar and vector
	 * paths on this same `fp16` probe (gadget_simd_selection.cpp,
	 * jit_compiler_floating.cpp). FPHP and ASIMDHP move together on this host
	 * because one probe answers for both.
	 */
	if (host.fp16)
		hwcap |= hwcap_fphp | hwcap_asimdhp;
	/* SQRDMLAH/SQRDMLSH. */
	if (host.has(ArmFeature::rdm))
		hwcap |= hwcap_asimdrdm;
	/* FMLAL/FMLSL. */
	if (host.has(ArmFeature::fhm))
		hwcap |= hwcap_asimdfhm;
	/* FJCVTZS. Some cores, such as the M1, set Z for a subnormal input that
	 * FPCR.FZ flushes, where the architecture clears it. The host probe
	 * `jscvt_subnormal_flags_correct` finds them, and both engines then
	 * correct the flags: the JIT after the native instruction, the gadget
	 * bank by taking its software path whenever FPCR.FZ is set. The
	 * instruction is therefore correct on every host that has it.
	 */
	if (host.has(ArmFeature::jscvt))
		hwcap |= hwcap_jscvt;
	/* FCADD/FCMLA. The gadget bank carries a dedicated complex family
	 * (`floating_complex_vector`, selected through `floating_complex_gadget`)
	 * and the JIT emits it natively. */
	if (host.has(ArmFeature::fcma))
		hwcap |= hwcap_fcma;
	/* LDAPR, and LDAPUR/STLUR for the offset forms. The gadget bank has a
	 * complete native RCpc set: `rcpc_load_gadgets` covers every size and both
	 * sign-extension shapes, `rcpc_store_gadgets` every store size. An acquire
	 * load the guest selects therefore stays on a fast path instead of falling
	 * back, which matters because this is what a C++ `memory_order_acquire`
	 * load lowers to. No Geekbench release emits these, so their value is in
	 * ordinary concurrent guest code rather than in a benchmark score. */
	if (host.has(ArmFeature::lrcpc))
		hwcap |= hwcap_lrcpc;
	if (host.has(ArmFeature::lrcpc2))
		hwcap |= hwcap_ilrcpc;
	/* CFINV, RMIF, SETF8 and SETF16, implemented in the gadget bank's
	 * core_integer.inc and executed correctly by both engines. */
	if (host.has(ArmFeature::flag_m))
		hwcap |= hwcap_flagm;
	/* FEAT_LSE2, advertised as USCAT: the guarantee that an LSE atomic on an
	 * address that is not naturally aligned is still single-copy atomic rather
	 * than a fault. No new encoding, so what is verified is the behaviour of
	 * the existing atomics on an unaligned address. */
	if (host.has(ArmFeature::lse2))
		hwcap |= hwcap_uscat;
	/* SB, the speculation barrier, which both translating engines lower. */
	if (host.has(ArmFeature::sb))
		hwcap |= hwcap_sb;
	/* FEAT_DIT: PSTATE.DIT is settable and readable through MSR/MRS. */
	if (host.has(ArmFeature::dit))
		hwcap |= hwcap_dit;
	/* AT_HWCAP2 keeps the same contract: advertise only what an engine has
	 * committed to executing, on the identical host predicate the native
	 * paths use.
	 *
	 * ABS, CNT, CTZ and the integer minimum and maximum, which both engines
	 * execute on every host: natively where the host has FEAT_CSSC, and
	 * otherwise as short sequences of base instructions. */
	unsigned long hwcap2 = hwcap2_cssc;
	/* CPY, CPYF and SET, which both engines execute on every host through
	 * one host memmove or memset per page region. */
	hwcap2 |= hwcap2_mops;
	/* SMMLA/UMMLA/USMMLA, reaching the native matrix emitters in both
	 * engines. */
	if (host.has(ArmFeature::i8mm))
		hwcap2 |= hwcap2_i8mm;
	/* FRINT32Z/FRINT64Z and friends. */
	if (host.has(ArmFeature::frintts))
		hwcap2 |= hwcap2_frint;
	/* BFDOT/BFMMLA/BFCVT. */
	if (host.has(ArmFeature::bf16))
		hwcap2 |= hwcap2_bf16;
	/* AXFLAG and XAFLAG, which convert between the Arm and alternate
	 * floating-point condition-flag encodings. */
	if (host.has(ArmFeature::flag_m2))
		hwcap2 |= hwcap2_flagm2;
	/* CNTVCTSS_EL0, which both engines read with the host's own
	 * instruction, so only a host with FEAT_ECV can offer it. */
	if (host.has(ArmFeature::ecv))
		hwcap2 |= hwcap2_ecv;
	/* FPCR.FIZ, AH and NEP, where both engines honour them exactly; the
	 * predicate names the host features that takes. */
	if (implements_afp(host))
		hwcap2 |= hwcap2_afp;
	/* The 12-bit FRECPE and FRSQRTE estimates under FPCR.AH, which both
	 * engines take from the host's own instructions. */
	if (implements_rpres(host))
		hwcap2 |= hwcap2_rpres;
	return {hwcap, hwcap2};
}

} // namespace uml::cpu::arm64::detail
