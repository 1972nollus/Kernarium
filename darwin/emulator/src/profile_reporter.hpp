// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace uml::emulator {

#if UML_ENABLE_JIT_PROFILE
/* Private app-facing facade, intentionally outside the installed C API. */
void report_jit_profiles() noexcept;
#endif

} // namespace uml::emulator
