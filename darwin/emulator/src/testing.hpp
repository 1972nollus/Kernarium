// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#if UML_EMULATOR_TESTING
#include "uml/emulator.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace uml::runtime {
class HibernationSteps;
}

namespace uml::emulator::testing {

using KernelEntry = int (*)(int argc, char *const argv[], char *const envp[]);

/* A kernel image that only the test variant runs. A handle created from it
 * takes every step of an embedded-kernel run, then calls `entry` with the
 * run's arguments and environment where the ELF loader would enter the
 * kernel, and returns its status. Any other image reaches the loader. */
[[nodiscard]] std::vector<std::byte> kernel_image(KernelEntry entry);

using AddressSpaceAdmissionHook = void (*)(void *) noexcept;

/* Test-only interleaving points for the process-wide address-space gate. */
void set_address_space_admission_hooks(
	AddressSpaceAdmissionHook after_handle_admission,
	AddressSpaceAdmissionHook at_wait_boundary,
	AddressSpaceAdmissionHook before_gate_release, void *context) noexcept;

/* Replaces the steps a hibernation takes, and what a failed restore does, on
 * `emulator`; the quiesce and resume calls then run the same ordering over
 * steps the test supplies. */
void use_hibernation(uml_emulator *emulator,
	std::unique_ptr<::uml::runtime::HibernationSteps> steps,
	void (*on_failure)() noexcept);

} // namespace uml::emulator::testing
#endif
