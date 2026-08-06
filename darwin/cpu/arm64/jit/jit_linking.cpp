// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_linking.hpp"

namespace uml::cpu::arm64 {

std::uint8_t checked_link_selector(std::uint8_t capabilities) noexcept {
  const auto selector = link_selector_for_capabilities(capabilities);
  /* Selector zero carries no setup capabilities. Every successor entry whose
   * requirements are not met is already null, so it safely exits to C++.
   * This also prevents an invalid host-owned mask from becoming an out-of-
   * bounds generated-code table read. */
  return selector == invalid_link_selector ? 0U : selector;
}

JitBlockLinkTable::JitBlockLinkTable(std::size_t capacity)
    : capacity_{capacity} {
  entries_.reserve(capacity);
}

JitBlockLink *JitBlockLinkTable::find_or_create(std::uint64_t pc) {
  if (const auto found = entries_.find(pc); found != entries_.end())
    return found->second.get();
  if (entries_.size() >= capacity_)
    return nullptr;
  auto entry = std::make_unique<JitBlockLink>();
  entry->guest_pc = pc;
  auto *result = entry.get();
  entries_.emplace(pc, std::move(entry));
  return result;
}

void JitBlockLinkTable::clear() noexcept { entries_.clear(); }

std::size_t JitBlockLinkTable::size() const noexcept {
  return entries_.size();
}

JitIndirectLinkTable::JitIndirectLinkTable(
    std::size_t capacity, const JitBlockLink *empty_link)
    : capacity_{capacity}, empty_link_{empty_link} {
  entries_.reserve(capacity);
}

JitIndirectLink *
JitIndirectLinkTable::find_or_create(std::uint64_t source_pc) {
  if (const auto found = entries_.find(source_pc); found != entries_.end())
    return found->second.get();
  if (entries_.size() >= capacity_)
    return nullptr;
  auto entry = std::make_unique<JitIndirectLink>();
  reset(*entry);
  entry->source_pc = source_pc;
  auto *result = entry.get();
  entries_.emplace(source_pc, std::move(entry));
  return result;
}

JitIndirectLink *
JitIndirectLinkTable::find(std::uint64_t source_pc) const noexcept {
  const auto found = entries_.find(source_pc);
  return found == entries_.end() ? nullptr : found->second.get();
}

void JitIndirectLinkTable::reset(JitIndirectLink &site) const noexcept {
  for (auto &entry : site.entries) {
    entry.guest_pc = 1;
    entry.link_entries = empty_link_->entries.data();
  }
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  site.profile_targets.fill(empty_link_);
#endif
  site.next_replacement = 0;
  site.activation_epoch = 0;
}

void JitIndirectLinkTable::clear() noexcept { entries_.clear(); }

std::size_t JitIndirectLinkTable::size() const noexcept {
  return entries_.size();
}

} // namespace uml::cpu::arm64
