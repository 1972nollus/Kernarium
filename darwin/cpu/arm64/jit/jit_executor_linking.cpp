// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_executor_internal.hpp"

#include "uml_darwin_static_mmu.h"

namespace uml::cpu::arm64 {

[[nodiscard]] void *JitExecutor::Implementation::execution_context(unsigned long generation) {
  if (execution_context_ != nullptr && execution_generation_ == generation)
    return execution_context_;
  darwin_uml_static_vm_execution_context_destroy(execution_context_);
  execution_context_ =
      darwin_uml_static_vm_execution_context_create(generation);
  execution_generation_ = execution_context_ != nullptr ? generation : 0;
  return execution_context_;
}

[[nodiscard]] const void *
JitExecutor::Implementation::resolve_block_link(void *context, std::uint64_t pc) {
  return static_cast<Implementation *>(context)->link_for(pc);
}

[[nodiscard]] const void *
JitExecutor::Implementation::resolve_indirect_link(void *context, std::uint64_t source_pc) {
  return static_cast<Implementation *>(context)->indirect_link_for(source_pc);
}

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
[[nodiscard]] std::uint64_t *
JitExecutor::Implementation::resolve_direct_link_profile(void *context, const void *target,
                            std::uint64_t source_pc,
                            std::uint32_t dirty_live_out) {
  return static_cast<Implementation *>(context)
      ->gpr_link_profile_.add_direct(
          static_cast<const JitBlockLink *>(target), source_pc,
          dirty_live_out);
}

[[nodiscard]] jit::JitIndirectLinkProfileCounters
JitExecutor::Implementation::resolve_indirect_link_profile(void *context, const void *site,
                              std::uint64_t source_pc,
                              std::uint32_t dirty_live_out) {
  return static_cast<Implementation *>(context)
      ->gpr_link_profile_.add_indirect(
          static_cast<const JitIndirectLink *>(site), source_pc,
          dirty_live_out);
}

#endif

[[nodiscard]] JitBlockLink *JitExecutor::Implementation::link_for(std::uint64_t pc) {
  auto *link = links_.find_or_create(pc);
#if UML_ENABLE_JIT_PROFILE
  profile_jit_link_table_state(links_.size(), links_.capacity(),
                               indirect_links_.size(),
                               indirect_links_.capacity());
#endif
#if UML_JIT_RUNTIME_DIAGNOSTICS
  if (links_.size() >
      cache_validation_diagnostics_.direct_link_table_high_water)
    cache_validation_diagnostics_.direct_link_table_high_water =
        links_.size();
#endif
  return link;
}

[[nodiscard]] JitIndirectLink *
JitExecutor::Implementation::indirect_link_for(std::uint64_t source_pc) {
  auto *site = indirect_links_.find_or_create(source_pc);
#if UML_ENABLE_JIT_PROFILE
  profile_jit_link_table_state(links_.size(), links_.capacity(),
                               indirect_links_.size(),
                               indirect_links_.capacity());
#endif
#if UML_JIT_RUNTIME_DIAGNOSTICS
  indirect_link_diagnostics_.capacity_refusals += site == nullptr;
  if (indirect_links_.size() > indirect_link_diagnostics_.table_high_water)
    indirect_link_diagnostics_.table_high_water = indirect_links_.size();
#endif
  return site;
}

void JitExecutor::Implementation::train_indirect(std::uint64_t source_pc,
                    const CompiledBlock &target) {
  auto *site = indirect_links_.find(source_pc);
  if (site == nullptr || target.link == nullptr ||
      target.link_epoch != link_epoch_ || !target.link_target_safe) {
#if UML_JIT_RUNTIME_DIAGNOSTICS
    ++indirect_link_diagnostics_.unsafe_target_refusals;
#endif
    return;
  }
  if (site->activation_epoch != link_epoch_) {
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    gpr_link_profile_.drain_indirect_site(*site);
#endif
    indirect_links_.reset(*site);
    site->activation_epoch = link_epoch_;
    active_indirect_links_.push_back(site);
  }
  for (std::size_t index = 0; index < site->entries.size(); ++index) {
    auto &entry = site->entries[index];
    if (entry.guest_pc != target.pc)
      continue;
    entry.link_entries = target.link->entries.data();
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    site->profile_targets[index] = target.link;
#endif
#if UML_JIT_RUNTIME_DIAGNOSTICS
    ++indirect_link_diagnostics_.trains;
#endif
    return;
  }
  const auto replacement = site->next_replacement & 1U;
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  gpr_link_profile_.drain_indirect_slot(*site, replacement);
#endif
  auto &entry = site->entries[replacement];
  /*
   * Publish the host-owned entry table before the comparison key. Execution
   * is single-threaded per executor, but this order also prevents a matching
   * key from ever exposing an uninitialized pointer during diagnostics.
   */
  entry.link_entries = target.link->entries.data();
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  site->profile_targets[replacement] = target.link;
#endif
  entry.guest_pc = target.pc;
  site->next_replacement ^= 1U;
#if UML_JIT_RUNTIME_DIAGNOSTICS
  ++indirect_link_diagnostics_.trains;
#endif
}

void JitExecutor::Implementation::deactivate_links() noexcept {
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  gpr_link_profile_.drain_direct();
#endif
  for (auto *link : active_links_) {
    link->entries.fill(0);
    link->instruction_count = 0;
    link->activation_epoch = 0;
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    link->profile_mask_valid = false;
#endif
  }
  active_links_.clear();
}

void JitExecutor::Implementation::deactivate_indirect_links() noexcept {
#if UML_JIT_RUNTIME_DIAGNOSTICS
  indirect_link_diagnostics_.deactivated_sites +=
      active_indirect_links_.size();
#endif
  for (auto *site : active_indirect_links_) {
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    gpr_link_profile_.drain_indirect_site(*site);
#endif
    indirect_links_.reset(*site);
  }
  active_indirect_links_.clear();
}

[[gnu::noinline]] void JitExecutor::Implementation::refresh_link(CompiledBlock &block) {
  if (block.link == nullptr) {
    block.link_epoch = link_epoch_;
    return;
  }
  auto &link = *block.link;
  link.instruction_count = block.instruction_count;
  link.entries.fill(0);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  link.profile_mask_valid = false;
#endif
  if (block.link_target_safe) {
    const auto requirements =
        link_state_requirements(block.accesses_fp_state, block.native_fp,
                                block.native_fpmr);
    const auto publication =
        link_capability(JitLinkCapability::store_publication);
    for (std::size_t index = 0; index < link.entries.size(); ++index) {
      const auto capabilities = jit_link_selector_capabilities[index];
      if ((capabilities & requirements) != requirements ||
          (block.direct_memory_write_mask != 0 &&
           (capabilities & publication) == 0U))
        continue;
      link.entries[index] = block.internal_entry;
    }
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    link.profile_read_before_write = block.profile_read_before_write;
    link.profile_mask_valid = true;
#endif
    if (link.activation_epoch != link_epoch_) {
      link.activation_epoch = link_epoch_;
      active_links_.push_back(&link);
    }
  }
  block.link_epoch = link_epoch_;
}

} // namespace uml::cpu::arm64
