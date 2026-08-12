// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_profile_reporter.hpp"

#include "cpu/arm64/jit/link_profile_api.hpp"

#if UML_ENABLE_JIT_PROFILE
namespace uml::runtime {

void report_jit_profiles() noexcept {
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  ::uml::cpu::arm64::report_global_gpr_link_profile();
#endif
  ::uml::cpu::arm64::report_jit_runtime_profiles();
}

} // namespace uml::runtime
#endif
