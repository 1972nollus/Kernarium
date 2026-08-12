// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace uml::runtime {

#if UML_ENABLE_JIT_PROFILE
/* Build-local quiescent reporting boundary. This is not installed ABI. */
void report_jit_profiles() noexcept;
#endif

} // namespace uml::runtime
