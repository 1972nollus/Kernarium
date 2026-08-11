// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_executor_internal.hpp"

namespace uml::cpu::arm64 {

JitExecutor::Implementation::Implementation(
    JitExecutorOptions options
#if UML_JIT_RUNTIME_DIAGNOSTICS
    , JitExecutorDiagnosticsOptions diagnostics
#endif
)
    : empty_link_{.guest_pc = 1},
      links_{maximum_block_links},
      indirect_links_{maximum_indirect_links, &empty_link_},
#if UML_JIT_RUNTIME_DIAGNOSTICS
      arena_{diagnostics.code_arena_size == 0
                 ? arena_size
                 : diagnostics.code_arena_size},
#else
      arena_{arena_size},
#endif
      enable_guarded_control_flow_{options.enable_guarded_control_flow},
      enable_pointer_authentication_{
          options.enable_pointer_authentication},
#if UML_JIT_RUNTIME_DIAGNOSTICS
      reuse_validated_stale_blocks_{
          diagnostics.reuse_validated_stale_blocks},
#endif
      host_capabilities_{options.host_capabilities} {
  active_links_.reserve(cache_sets * cache_ways);
  active_indirect_links_.reserve(cache_sets * cache_ways);
  register_jit_store_participant(store_participant_);
  SharedTranslationCache::instance().add_executor();
}

JitExecutor::Implementation::~Implementation() {
  if (current_cpu_ != nullptr)
    arm64::restore_native_fp_environment(*current_cpu_);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  gpr_link_profile_.publish();
#endif
  release_current_reservation();
  unregister_jit_store_participant(store_participant_);
  SharedTranslationCache::instance().remove_executor();
  darwin_uml_static_vm_execution_context_destroy(execution_context_);
}

[[nodiscard]] bool JitExecutor::Implementation::begin_execution(unsigned long generation,
                                   std::uint64_t guest_pc) {
  auto *context = execution_context(generation);
  return context != nullptr &&
         darwin_uml_static_vm_execution_context_begin(context, guest_pc) == 0;
}

void JitExecutor::Implementation::end_execution() noexcept {
  pending_indirect_ = {};
  if (current_cpu_ != nullptr)
    arm64::restore_native_fp_environment(*current_cpu_);
  release_current_reservation();
  current_cpu_ = nullptr;
  if (execution_context_ != nullptr)
    (void)darwin_uml_static_vm_execution_context_end(execution_context_);
}

[[nodiscard]] bool JitExecutor::Implementation::preempted() const noexcept { return preempted_; }

[[nodiscard]] std::uint64_t JitExecutor::Implementation::fault_address() const noexcept {
  return tlb_.fault_address;
}

[[nodiscard]] Access JitExecutor::Implementation::fault_access() const noexcept {
  return tlb_.fault_access;
}

[[nodiscard]] std::uint32_t JitExecutor::Implementation::floating_point_syndrome() const noexcept {
  return floating_point_syndrome_;
}

#if UML_JIT_RUNTIME_DIAGNOSTICS
[[nodiscard]] JitCacheValidationDiagnostics
JitExecutor::Implementation::cache_validation_diagnostics() const noexcept {
  return cache_validation_diagnostics_;
}

[[nodiscard]] JitIndirectLinkDiagnostics
JitExecutor::Implementation::indirect_link_diagnostics() const noexcept {
  return indirect_link_diagnostics_;
}
#endif

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
[[nodiscard]] JitGprLinkProfileDiagnostics
JitExecutor::Implementation::gpr_link_profile_diagnostics() noexcept {
  return gpr_link_profile_.diagnostics();
}

#endif

JitExecutor::JitExecutor(bool enable_guarded_control_flow)
    : JitExecutor{JitExecutorOptions{
          .enable_guarded_control_flow = enable_guarded_control_flow}} {}

JitExecutor::JitExecutor(JitExecutorOptions options)
    : implementation_{std::make_unique<Implementation>(
          options
#if UML_JIT_RUNTIME_DIAGNOSTICS
          , JitExecutorDiagnosticsOptions{}
#endif
      )} {}

#if UML_JIT_RUNTIME_DIAGNOSTICS
JitExecutor::JitExecutor(JitExecutorOptions options,
                         JitExecutorDiagnosticsOptions diagnostics)
    : implementation_{
          std::make_unique<Implementation>(options, diagnostics)} {}
#endif

JitExecutor::~JitExecutor() = default;

bool JitExecutor::begin_execution(unsigned long generation,
                                  std::uint64_t guest_pc) {
  return implementation_->begin_execution(generation, guest_pc);
}

void JitExecutor::end_execution() noexcept { implementation_->end_execution(); }

Event JitExecutor::run_block(CpuState &cpu, StaticGuestMemory &memory,
                             unsigned long generation, std::size_t &executed) {
  return implementation_->run_block(cpu, memory, generation, executed);
}

bool JitExecutor::preempted() const noexcept {
  return implementation_->preempted();
}

std::uint64_t JitExecutor::fault_address() const noexcept {
  return implementation_->fault_address();
}

Access JitExecutor::fault_access() const noexcept {
  return implementation_->fault_access();
}

std::uint32_t JitExecutor::floating_point_syndrome() const noexcept {
  return implementation_->floating_point_syndrome();
}

#if UML_JIT_RUNTIME_DIAGNOSTICS
JitCacheValidationDiagnostics
JitExecutor::cache_validation_diagnostics() const noexcept {
  return implementation_->cache_validation_diagnostics();
}

JitIndirectLinkDiagnostics
JitExecutor::indirect_link_diagnostics() const noexcept {
  return implementation_->indirect_link_diagnostics();
}
#endif

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
JitGprLinkProfileDiagnostics
JitExecutor::gpr_link_profile_diagnostics() noexcept {
  return implementation_->gpr_link_profile_diagnostics();
}

JitGprLinkProfileDiagnostics
global_gpr_link_profile_diagnostics() noexcept {
  return JitGprLinkProfileGlobal::instance().diagnostics();
}

void report_global_gpr_link_profile() noexcept {
  JitGprLinkProfileGlobal::instance().report();
}
#endif

bool native_jit_available() noexcept {
  return jit::detail::ExecutableArena::available();
}

} // namespace uml::cpu::arm64

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
extern "C" void uml_emulator_report_jit_gpr_link_profile(void) {
  uml::cpu::arm64::report_global_gpr_link_profile();
}
#endif
