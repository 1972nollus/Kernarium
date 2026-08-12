// SPDX-License-Identifier: GPL-2.0-or-later
#include "link_profile_api.hpp"

#include "jit_diagnostics.hpp"
#include "jit_executor_pool_profile.hpp"
#include "jit_shared_translations.hpp"

#if UML_ENABLE_JIT_PROFILE
namespace uml::cpu::arm64 {

void report_jit_runtime_profiles() noexcept {
  report_final_jit_diagnostics();
  report_final_jit_executor_pool_profile();
  report_final_shared_translation_statistics();
}

} // namespace uml::cpu::arm64
#endif
