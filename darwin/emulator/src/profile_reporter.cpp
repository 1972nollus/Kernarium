// SPDX-License-Identifier: GPL-2.0-or-later
#include "profile_reporter.hpp"

#include "runtime/jit_profile_reporter.hpp"

#if UML_ENABLE_JIT_PROFILE
namespace uml::emulator {

void report_jit_profiles() noexcept {
  ::uml::runtime::report_jit_profiles();
}

} // namespace uml::emulator
#endif
