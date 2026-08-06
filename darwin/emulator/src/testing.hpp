// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#if UML_EMULATOR_ADMISSION_TESTING
namespace uml::emulator::testing {

using AddressSpaceAdmissionHook = void (*)(void *) noexcept;

/* Test-only interleaving points for the process-wide address-space gate. */
void set_address_space_admission_hooks(
	AddressSpaceAdmissionHook after_handle_admission,
	AddressSpaceAdmissionHook at_wait_boundary,
	AddressSpaceAdmissionHook before_gate_release, void *context) noexcept;

/* Force the next blocked process-address-space wait to fail as if the host
 * condition variable rejected the wait operation. */
void fail_next_address_space_admission_wait() noexcept;

} // namespace uml::emulator::testing
#endif
