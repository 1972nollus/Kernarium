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
constexpr unsigned long hwcap_sha3 = 1UL << 17;
constexpr unsigned long hwcap_sha512 = 1UL << 21;
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
	/* No HWCAP2 extension is part of the current virtual CPU contract. */
	return 0;
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
