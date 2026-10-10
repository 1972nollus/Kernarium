// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml_darwin_cpu.h"

#include "cpu/arm64/id_registers.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/arm64/sve_profile.hpp"
#include "cpu/guest_identity.hpp"
#include "platform/apple/host_features.hpp"
#include "runtime/context.hpp"

/* The HWCAPs follow the running engine's profile (guest_host_capabilities),
 * as its identification registers do. */
extern "C" unsigned long darwin_uml_guest_hwcap(void)
{
	return ::uml::cpu::arm64::detail::guest_hwcaps(
		::uml::runtime::current_guest_host_capabilities())[0];
}

extern "C" const char *darwin_uml_guest_cpu_model_name(void)
{
	/*
	 * The kernel prints what the emulator decides this CPU is, rather than
	 * naming a CPU of its own. An arm64 slice and the x86-64 userspace
	 * slice therefore report the same machine, and on x86-64 this is the
	 * same name the CPUID brand string carries.
	 */
	return ::uml::cpu::guest_cpu_model_name_storage;
}

extern "C" unsigned long darwin_uml_guest_hwcap2(void)
{
	return ::uml::cpu::arm64::detail::guest_hwcaps(
		::uml::runtime::current_guest_host_capabilities())[1];
}

/* Matches the register file every thread's execution state sizes. */
extern "C" unsigned long darwin_uml_guest_sme_vector_length(void)
{
#if DARWIN_UML_X86_64_USERSPACE_ONLY
	return 0;
#else
	const auto &host = ::uml::platform::apple::probed_host_capabilities();
	return ::uml::cpu::arm64::implements_sme(host) ? host.sme_svl_bytes : 0;
#endif
}

/* From the running engine's profile, as the HWCAPs: non-zero exactly when
 * they advertise SVE, which an engine does only at sve_support complete. */
extern "C" unsigned long darwin_uml_guest_sve_vector_length(void)
{
#if DARWIN_UML_X86_64_USERSPACE_ONLY
	return 0;
#else
	return ::uml::cpu::arm64::implements_sve(
		::uml::runtime::current_guest_host_capabilities()) ?
		::uml::cpu::arm64::sve_vector_length : 0;
#endif
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
