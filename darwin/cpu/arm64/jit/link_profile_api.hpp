// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* Build-local diagnostics used by the macOS CLI's opt-in profiling build.
 * This is not part of the installed emulator API or its ABI identity. */
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
extern "C" void uml_emulator_report_jit_gpr_link_profile(void);
#endif
