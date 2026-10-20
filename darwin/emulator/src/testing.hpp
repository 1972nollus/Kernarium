// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#if UML_EMULATOR_TESTING
#include "uml/emulator.h"

#include <cstddef>
#include <cstdint>
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

/* Places the native kernel window of every later run at `start`, at the size
 * the run derives, instead of at the product's address; 0 restores the
 * product's placement. A sanitizer holds the macOS product window for itself,
 * so a sanitized test hands runs a window the sanitizer leaves free. A window
 * the host reserves, as on an iOS device, stays where the host put it. */
void place_native_kernel_window(std::uintptr_t start) noexcept;

struct NativeKernelWindow final {
	std::uintptr_t start{};
	std::uintptr_t end{};
};

/* The native kernel window a run with `guest_memory` bytes of guest RAM takes;
 * empty when the run would refuse that size. */
[[nodiscard]] NativeKernelWindow native_kernel_window(
	std::size_t guest_memory) noexcept;

/* Replaces the steps a hibernation takes, and what a failed restore does, on
 * `emulator`; the quiesce and resume calls then run the same ordering over
 * steps the test supplies. */
void use_hibernation(uml_emulator *emulator,
	std::unique_ptr<::uml::runtime::HibernationSteps> steps,
	void (*on_failure)() noexcept);

} // namespace uml::emulator::testing
#endif
