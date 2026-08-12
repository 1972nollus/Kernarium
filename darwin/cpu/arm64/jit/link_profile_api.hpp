// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* Build-local diagnostics used by the macOS CLI's opt-in profiling builds.
 * These are not part of the installed emulator API or its ABI identity. */
namespace uml::cpu::arm64 {

#if UML_ENABLE_JIT_PROFILE
/* The caller must have quiesced translation and retired its task carriers. The
 * report drains live thread-local recorders and directly emits each enabled
 * summary whose cumulative state changed since the preceding call. No stdio
 * flush is required for these descriptor writes.
 */
void report_jit_runtime_profiles() noexcept;
#endif

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
void report_global_gpr_link_profile() noexcept;
#endif

} // namespace uml::cpu::arm64
