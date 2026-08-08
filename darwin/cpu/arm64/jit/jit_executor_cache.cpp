// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_executor_internal.hpp"

namespace uml::cpu::arm64 {

[[nodiscard]] JitCacheValidationResult JitExecutor::Implementation::validate_stale_block(
    const CompiledBlock &block, const SavedGuestWords &guest_words,
    std::uint64_t changes,
    StaticGuestMemory &memory) {
  if (block.instruction_count == 0 ||
      block.instruction_count > guest_words.size())
    return JitCacheValidationResult::mismatch;

  /*
   * This runs on every stale hit in production builds, and it used to do the
   * job in the slowest available shape: one virtual fetch through the software
   * TLB per instruction, plus a four-byte memcmp, up to 64 times, for at most
   * 256 contiguous bytes. Fetch page-bounded runs instead and compare each run
   * once, which is the same page-splitting loop fetch_instruction_window()
   * already uses in the gadget executor.
   *
   * Fault reporting has to stay exactly as it was: the old loop returned
   * mismatch as soon as a word differed, and only reported fault for an
   * unreadable word it actually reached. A bulk fetch cannot tell which of
   * those applies when it fails, so a failed run falls back to the
   * word-at-a-time path over that run rather than reporting either outcome
   * for the whole span.
   */
  constexpr std::uint64_t guest_page_size = DARWIN_UML_GUEST_PAGE_SIZE;
  SavedGuestWords fetched{};
  std::size_t index = 0;
  while (index < block.instruction_count) {
    const auto address = block.pc + index * sizeof(std::uint32_t);
    const auto page_remaining =
        guest_page_size - (address & (guest_page_size - 1));
    auto run = std::min<std::size_t>(
        block.instruction_count - index,
        static_cast<std::size_t>(page_remaining / sizeof(std::uint32_t)));
    /* A valid A64 pc is word aligned; keep a malformed one deterministic
     * rather than constructing an empty span. */
    if (run == 0)
      run = 1;
    const auto words = std::span{fetched}.subspan(index, run);
    if (memory.fetch(address, std::as_writable_bytes(words))) {
      if (std::memcmp(words.data(), &guest_words[index],
                      run * sizeof(std::uint32_t)) != 0)
        return JitCacheValidationResult::mismatch;
      index += run;
      continue;
    }
    for (std::size_t step = 0; step < run; ++step) {
      std::array<std::byte, sizeof(std::uint32_t)> word{};
      if (!memory.fetch(address + step * sizeof(std::uint32_t), word))
        return JitCacheValidationResult::fault;
      if (std::memcmp(word.data(), &guest_words[index + step],
                      word.size()) != 0)
        return JitCacheValidationResult::mismatch;
    }
    index += run;
  }

  /*
   * The exact-byte proof is useful only for the sequence whose transition
   * caused this lookup. A concurrent code publication after the first
   * sequence snapshot must leave the old link inactive and retry through
   * the normal miss path.
   */
  const auto current =
      darwin_uml_static_vm_execution_context_sequences(execution_context_);
  if (current.execution_interrupt != 0 ||
      current.mapping_changes != mapping_changes_ ||
      current.code_changes != changes)
    return JitCacheValidationResult::sequence_race;
  return JitCacheValidationResult::hit;
}

[[nodiscard]] CompiledBlock *JitExecutor::Implementation::find(unsigned long generation,
                                  std::uint64_t changes, std::uint64_t pc,
                                  StaticGuestMemory &memory) {
  const auto set_index = cache_index(generation, pc);
  auto &set = (*block_cache_)[set_index];
  auto &saved = (*saved_guest_words_)[set_index];
  CompiledBlock *stale{};
  std::size_t stale_way{};
  for (std::size_t way = 0; way < set.ways.size(); ++way) {
    auto &block = set.ways[way];
    if (block.function != nullptr && block.generation == generation &&
        block.code_changes == changes && block.pc == pc) {
      block.stamp = ++stamp_;
      activate_link(block);
      return &block;
    }
    if (block.function != nullptr && block.generation == generation &&
        block.pc == pc &&
        (stale == nullptr || block.stamp > stale->stamp)) {
      stale = &block;
      stale_way = way;
    }
  }
#if UML_JIT_RUNTIME_DIAGNOSTICS
  if (!reuse_validated_stale_blocks_ || stale == nullptr)
#else
  if (stale == nullptr)
#endif
    return nullptr;

#if UML_JIT_RUNTIME_DIAGNOSTICS
  ++cache_validation_diagnostics_.validation_attempts;
#endif
  const auto validation =
      validate_stale_block(*stale, saved.ways[stale_way], changes, memory);
  profile_jit_cache_validation(validation);
  switch (validation) {
  case JitCacheValidationResult::hit:
#if UML_JIT_RUNTIME_DIAGNOSTICS
    ++cache_validation_diagnostics_.validation_hits;
#endif
    stale->code_changes = changes;
    stale->stamp = ++stamp_;
    ++compiled_in_link_sequence_;
    activate_link(*stale);
    return stale;
  case JitCacheValidationResult::mismatch:
#if UML_JIT_RUNTIME_DIAGNOSTICS
    ++cache_validation_diagnostics_.validation_mismatches;
#endif
    *stale = {};
    break;
  case JitCacheValidationResult::fault:
#if UML_JIT_RUNTIME_DIAGNOSTICS
    ++cache_validation_diagnostics_.validation_faults;
#endif
    *stale = {};
    break;
  case JitCacheValidationResult::sequence_race:
#if UML_JIT_RUNTIME_DIAGNOSTICS
    ++cache_validation_diagnostics_.validation_sequence_races;
#endif
    break;
  }
  return nullptr;
}

[[nodiscard]] CacheSlot JitExecutor::Implementation::replacement(unsigned long generation,
                                    std::uint64_t changes,
                                    std::uint64_t pc) {
  const auto set_index = cache_index(generation, pc);
  auto &set = (*block_cache_)[set_index];
  std::size_t selected_way = 0;
  for (std::size_t way = 0; way < set.ways.size(); ++way)
    if (set.ways[way].function == nullptr ||
        set.ways[way].stamp < set.ways[selected_way].stamp)
      selected_way = way;
  auto *selected = &set.ways[selected_way];
  const bool eviction = selected->function != nullptr;
  profile_jit_cache_replacement(eviction);
#if UML_JIT_RUNTIME_DIAGNOSTICS
  cache_validation_diagnostics_.cache_replacements += eviction;
#endif
  *selected = {};
  selected->generation = generation;
  selected->code_changes = changes;
  selected->pc = pc;
  selected->stamp = ++stamp_;
  return {
      .block = selected,
      .guest_words =
          &(*saved_guest_words_)[set_index].ways[selected_way],
  };
}

CompiledBlock &JitExecutor::Implementation::store_native(unsigned long generation, std::uint64_t changes,
                            std::uint64_t pc, JitFunction function,
                            std::uintptr_t internal_entry,
                            std::span<const std::uint32_t> guest_words,
                            std::uint64_t memory_write_mask,
                            std::uint64_t direct_memory_write_mask,
                            bool accesses_fp_state, bool link_target_safe,
                            bool native_fp, bool native_fpmr,
                            bool guest_illegal
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
                            ,
                            std::uint32_t profile_read_before_write
#endif
) {
  /* Native FP/FPMR state always implies materialization. The executor already
   * validates compiler metadata before installation; keep this boundary safe
   * as well if a future caller omits that validation. */
  accesses_fp_state = accesses_fp_state || native_fp || native_fpmr;
  const auto selected = replacement(generation, changes, pc);
  auto &block = *selected.block;
  block.function = function;
  block.internal_entry = internal_entry;
  block.link = link_for(pc);
  block.instruction_count = guest_words.size();
  std::copy(guest_words.begin(), guest_words.end(),
            selected.guest_words->begin());
  block.memory_write_mask = memory_write_mask;
  block.direct_memory_write_mask = direct_memory_write_mask;
  block.accesses_fp_state = accesses_fp_state;
  block.link_target_safe = link_target_safe;
  block.native_fp = native_fp;
  block.native_fpmr = native_fpmr;
  block.guest_illegal = guest_illegal;
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  block.profile_read_before_write = profile_read_before_write;
#endif
#if UML_JIT_RUNTIME_DIAGNOSTICS
  ++cache_validation_diagnostics_.translations;
#endif
  ++compiled_in_link_sequence_;
  activate_link(block);
  return block;
}

void JitExecutor::Implementation::clear_cache() noexcept {
  for (auto &set : *block_cache_)
    set = {};
  deactivate_indirect_links();
  deactivate_links();
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  gpr_link_profile_.clear_code();
#endif
  links_.clear();
  indirect_links_.clear();
  pending_indirect_ = {};
  tracked_store_arena_.reset();
  link_generation_ = 0;
  link_code_changes_ = 0;
  compiled_in_link_sequence_ = 0;
  link_sequence_initialized_ = false;
}

} // namespace uml::cpu::arm64
