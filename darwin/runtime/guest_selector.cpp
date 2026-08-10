// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml_darwin_cpu.h"

#include "platform/apple/host_features.hpp"
#include "runtime/context.hpp"

namespace {

/* Keep synchronized with linux/arch/arm64/um/asm/elf.h and the
 * dispatcher/gadget instruction profile.  In particular, do not forward the
 * host's complete feature set: doing so would let libc generate instructions
 * that no guest engine has promised to implement.
 */
constexpr unsigned long hwcap_fp = 1UL << 0;
constexpr unsigned long hwcap_asimd = 1UL << 1;
constexpr unsigned long hwcap_aes = 1UL << 3;
constexpr unsigned long hwcap_pmull = 1UL << 4;
constexpr unsigned long hwcap_sha1 = 1UL << 5;
constexpr unsigned long hwcap_sha2 = 1UL << 6;
constexpr unsigned long hwcap_crc32 = 1UL << 7;
constexpr unsigned long hwcap_atomics = 1UL << 8;
constexpr unsigned long hwcap_fphp = 1UL << 9;
constexpr unsigned long hwcap_asimdhp = 1UL << 10;
constexpr unsigned long hwcap_asimdrdm = 1UL << 12;
constexpr unsigned long hwcap_jscvt = 1UL << 13;
constexpr unsigned long hwcap_fcma = 1UL << 14;
constexpr unsigned long hwcap_lrcpc = 1UL << 15;
constexpr unsigned long hwcap_sha3 = 1UL << 17;
constexpr unsigned long hwcap_asimddp = 1UL << 20;
constexpr unsigned long hwcap_sha512 = 1UL << 21;
constexpr unsigned long hwcap_asimdfhm = 1UL << 23;
constexpr unsigned long hwcap_dit = 1UL << 24;
constexpr unsigned long hwcap_uscat = 1UL << 25;
constexpr unsigned long hwcap_ilrcpc = 1UL << 26;
constexpr unsigned long hwcap_flagm = 1UL << 27;
constexpr unsigned long hwcap_sb = 1UL << 29;
constexpr unsigned long hwcap2_flagm2 = 1UL << 7;
constexpr unsigned long hwcap2_frint = 1UL << 8;
constexpr unsigned long hwcap2_i8mm = 1UL << 13;
constexpr unsigned long hwcap2_bf16 = 1UL << 14;
[[nodiscard]] unsigned long virtual_hwcap() noexcept
{
	const auto &host = ::uml::platform::apple::probed_host_capabilities();
	unsigned long result = hwcap_fp | hwcap_asimd;
	if (host.aes)
		result |= hwcap_aes;
	if (host.pmull)
		result |= hwcap_pmull;
	if (host.sha1)
		result |= hwcap_sha1;
	if (host.sha2)
		result |= hwcap_sha2;
	if (host.crc32)
		result |= hwcap_crc32;
	if (host.has(::uml::platform::ArmFeature::lse))
		result |= hwcap_atomics;
	if (host.sha3)
		result |= hwcap_sha3;
	if (host.sha512)
		result |= hwcap_sha512;
	/* SDOT/UDOT/USDOT. All three engines execute this family: the gadget bank
	 * selects native dot gadgets behind `ArmFeature::dotprod`
	 * (gadget_memory_selection.cpp), the JIT emits it natively behind the same
	 * predicate (JitCompilerFamilies::emit_simd_dot), and the interpreter
	 * implements it in software through Dispatcher::simd_dot_product. Gating
	 * the advertisement on the identical host predicate the two native paths
	 * use keeps the promise exact: guest libc and guest JITs may select these
	 * opcodes only where an engine has committed to executing them.
	 */
	if (host.has(::uml::platform::ArmFeature::dotprod))
		result |= hwcap_asimddp;
	/* Half precision. Both engines gate their native 16-bit scalar and vector
	 * paths on this same `fp16` probe (gadget_simd_selection.cpp,
	 * jit_compiler_floating.cpp). FPHP and ASIMDHP move together on this host
	 * because one probe answers for both.
	 */
	if (host.fp16)
		result |= hwcap_fphp | hwcap_asimdhp;
	/* SQRDMLAH/SQRDMLSH. */
	if (host.has(::uml::platform::ArmFeature::rdm))
		result |= hwcap_asimdrdm;
	/* FMLAL/FMLSL. */
	if (host.has(::uml::platform::ArmFeature::fhm))
		result |= hwcap_asimdfhm;
	/* FJCVTZS. The host probe additionally verifies that this core raises the
	 * subnormal flags the architecture requires; without that the engines
	 * decline the native encoding, so the guest must not be told it exists.
	 */
	if (host.has(::uml::platform::ArmFeature::jscvt) &&
	    host.jscvt_subnormal_flags_correct)
		result |= hwcap_jscvt;
	/* FCADD/FCMLA. The gadget bank carries a dedicated complex family
	 * (`floating_complex_vector`, selected through `floating_complex_gadget`)
	 * and the JIT emits it natively. */
	if (host.has(::uml::platform::ArmFeature::fcma))
		result |= hwcap_fcma;
	/* LDAPR, and LDAPUR/STLUR for the offset forms. The gadget bank has a
	 * complete native RCpc set: `rcpc_load_gadgets` covers every size and both
	 * sign-extension shapes, `rcpc_store_gadgets` every store size. An acquire
	 * load the guest selects therefore stays on a fast path instead of falling
	 * back, which matters because this is what a C++ `memory_order_acquire`
	 * load lowers to. No Geekbench release emits these, so their value is in
	 * ordinary concurrent guest code rather than in a benchmark score. */
	if (host.has(::uml::platform::ArmFeature::lrcpc))
		result |= hwcap_lrcpc;
	if (host.has(::uml::platform::ArmFeature::lrcpc2))
		result |= hwcap_ilrcpc;
	/* CFINV, RMIF, SETF8 and SETF16, implemented in the gadget bank's
	 * core_integer.inc and executed correctly by all three engines. */
	if (host.has(::uml::platform::ArmFeature::flag_m))
		result |= hwcap_flagm;
	/* FEAT_LSE2, advertised as USCAT: the guarantee that an LSE atomic on an
	 * address that is not naturally aligned is still single-copy atomic rather
	 * than a fault. No new encoding, so what is verified is the behaviour of
	 * the existing atomics on an unaligned address. */
	if (host.has(::uml::platform::ArmFeature::lse2))
		result |= hwcap_uscat;
	/* SB, the speculation barrier, which both translating engines lower. */
	if (host.has(::uml::platform::ArmFeature::sb))
		result |= hwcap_sb;
	/* FEAT_DIT: PSTATE.DIT is settable and readable through MSR/MRS. */
	if (host.has(::uml::platform::ArmFeature::dit))
		result |= hwcap_dit;
	return result;
}

} // namespace

extern "C" unsigned long darwin_uml_guest_hwcap(void)
{
	/*
	 * Production gadget programs use native host crypto operations.  Expose
	 * only extensions that are both implemented by the virtual CPU and safe
	 * on this host; never let libc select an opcode that would SIGILL inside
	 * the gadget or JIT engine.
	 */
	return virtual_hwcap();
}

extern "C" unsigned long darwin_uml_guest_hwcap2(void)
{
	/*
	 * Same contract as HWCAP: advertise only what an engine has committed to
	 * executing, on the identical host predicate the native paths use.
	 */
	const auto &host = ::uml::platform::apple::probed_host_capabilities();
	unsigned long result = 0;
	/* SMMLA/UMMLA/USMMLA, reaching Dispatcher::simd_matrix_multiply in the
	 * interpreter and the native matrix emitters in the other two engines. */
	if (host.has(::uml::platform::ArmFeature::i8mm))
		result |= hwcap2_i8mm;
	/* FRINT32Z/FRINT64Z and friends. */
	if (host.has(::uml::platform::ArmFeature::frintts))
		result |= hwcap2_frint;
	/* BFDOT/BFMMLA/BFCVT. */
	if (host.has(::uml::platform::ArmFeature::bf16))
		result |= hwcap2_bf16;
	/* AXFLAG and XAFLAG, which convert between the Arm and alternate
	 * floating-point condition-flag encodings. */
	if (host.has(::uml::platform::ArmFeature::flag_m2))
		result |= hwcap2_flagm2;
	return result;
}

extern "C" int darwin_uml_enter_guest(void *registers, void *fp_registers,
				       unsigned long fp_size,
				       unsigned long generation,
				       unsigned long *tls)
{
	auto *context = ::uml::runtime::current_context();
	if (context == nullptr)
		return DARWIN_UML_EVENT_FATAL;
	const auto entry = context->guest_entry();
	return entry(registers, fp_registers, fp_size, generation, tls);
}
