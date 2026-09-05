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
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  return profile_links_.find_or_create(pc, link_generation_);
#else
  (void)pc;
  /* The literal is replaced with the private table's unique target anchor before
   * the code is published. A nonnull sentinel asks the compiler for the
   * linked shape without allocating target-owned storage during lowering.
   */
  return &empty_link_;
#endif
}

[[nodiscard]] JitIndirectLink *
JitExecutor::Implementation::indirect_link_for(std::uint64_t source_pc) {
  auto *site = indirect_links_.find_or_create(source_pc, link_generation_);
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
  auto *site = indirect_links_.find(source_pc, link_generation_);
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
    site->next_active = link_context_->active_indirect_links;
    link_context_->active_indirect_links = site;
  }
  for (std::size_t index = 0; index < site->entries.size(); ++index) {
    auto &entry = site->entries[index];
    if (entry.guest_pc != target.pc)
      continue;
    entry.link = target.link;
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
  entry.link = target.link;
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  site->profile_targets[replacement] = target.link;
#endif
  entry.guest_pc = target.pc;
  site->next_replacement ^= 1U;
#if UML_JIT_RUNTIME_DIAGNOSTICS
  ++indirect_link_diagnostics_.trains;
#endif
}

void JitExecutor::Implementation::discard_links() noexcept {
  if (link_context_ == nullptr)
    return;
  link_context_->active_links = nullptr;
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  gpr_link_profile_.drain_direct();
  for (auto *link = link_context_->active_profile_links; link != nullptr;
       link = link->next_active) {
    link->internal_entry = 0;
    link->allowed_selectors = 0;
    link->instruction_count = 0;
    link->activation_epoch = 0;
    link->profile_mask_valid = false;
  }
  link_context_->active_profile_links = nullptr;
#endif
}

void JitExecutor::Implementation::deactivate_links() noexcept {
  if (link_context_ == nullptr)
    return;
  for (auto *link = link_context_->active_links; link != nullptr;
       link = link->next_active)
    arena_.deactivate_links({&link, 1});
  discard_links();
}

void JitExecutor::Implementation::deactivate_indirect_links() noexcept {
  if (link_context_ == nullptr)
    return;
  for (auto *site = link_context_->active_indirect_links; site != nullptr;
       site = site->next_active) {
#if UML_JIT_RUNTIME_DIAGNOSTICS
    ++indirect_link_diagnostics_.deactivated_sites;
#endif
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
    gpr_link_profile_.drain_indirect_site(*site);
#endif
    indirect_links_.reset(*site);
  }
  link_context_->active_indirect_links = nullptr;
}

[[gnu::noinline]] void JitExecutor::Implementation::refresh_link(CompiledBlock &block) {
  if (block.link == nullptr) {
    block.link_epoch = link_epoch_;
    return;
  }
  JitBlockLinkActivation activation{
      .instruction_count = block.instruction_count,
      .epoch = link_epoch_,
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
      .profile_read_before_write = block.profile_read_before_write,
      .profile_mask_valid = block.link_target_safe,
#endif
  };
  const auto first_activation =
      block.link != nullptr && block.link->activation_epoch != link_epoch_;
  if (block.link_target_safe) {
    const auto requirements =
        link_state_requirements(block.accesses_fp_state, block.native_fp,
                                block.native_fpmr);
    const auto publication =
        link_capability(JitLinkCapability::store_publication);
    for (std::size_t index = 0;
         index < jit_link_selector_capabilities.size(); ++index) {
      const auto capabilities = jit_link_selector_capabilities[index];
      if ((capabilities & requirements) != requirements ||
          (block.direct_memory_write_mask != 0 &&
           (capabilities & publication) == 0U))
        continue;
      activation.allowed_selectors |= std::uint32_t{1} << index;
    }
    activation.internal_entry = block.internal_entry;
  }
  arena_.activate_link(*block.link, activation);
  if (activation.allowed_selectors != 0 && first_activation) {
    block.link->next_active = link_context_->active_links;
    link_context_->active_links = block.link;
  }
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  if (block.profile_link != nullptr) {
    auto &profile_link = *block.profile_link;
    profile_link.instruction_count = activation.instruction_count;
    profile_link.internal_entry = activation.internal_entry;
    profile_link.allowed_selectors = activation.allowed_selectors;
    profile_link.profile_read_before_write =
        activation.profile_read_before_write;
    profile_link.profile_mask_valid = activation.profile_mask_valid;
    if (profile_link.activation_epoch != link_epoch_) {
      profile_link.activation_epoch = link_epoch_;
      profile_link.next_active = link_context_->active_profile_links;
      link_context_->active_profile_links = &profile_link;
    }
  }
#endif
  block.link_epoch = link_epoch_;
}

void JitExecutor::Implementation::refresh_cached_link_target(
    std::uint64_t pc) {
  if (link_generation_ == 0)
    return;
  auto &set = (*block_cache_)[cache_index(
      link_generation_, pc, geometry_.cache_sets)];
  for (auto &block : set.ways) {
    if (block.function == nullptr || block.generation != link_generation_ ||
        block.code_changes != link_code_changes_ || block.pc != pc)
      continue;
    refresh_link(block);
    return;
  }
}

} // namespace uml::cpu::arm64
