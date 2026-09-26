// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_executor_internal.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <span>

namespace uml::cpu::arm64 {

[[nodiscard]] JitCacheValidationResult
JitExecutor::Implementation::validate_stale_block(
    const CompiledBlock &block, std::uint64_t changes,
    StaticGuestMemory &memory) {
  if (block.instruction_count == 0 ||
      block.instruction_count > maximum_block_instructions ||
      block.saved_guest_words == nullptr)
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
  GuestWordBuffer fetched{};
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
      if (std::memcmp(words.data(), &block.saved_guest_words[index],
                      run * sizeof(std::uint32_t)) != 0)
        return JitCacheValidationResult::mismatch;
      index += run;
      continue;
    }
    for (std::size_t step = 0; step < run; ++step) {
      std::array<std::byte, sizeof(std::uint32_t)> word{};
      if (!memory.fetch(address + step * sizeof(std::uint32_t), word))
        return JitCacheValidationResult::fault;
      if (std::memcmp(word.data(),
                      &block.saved_guest_words[index + step],
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
  const auto set_index = cache_index(generation, pc, geometry_.cache_sets);
  auto &set = (*block_cache_)[set_index];
  CompiledBlock *stale{};
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
      validate_stale_block(*stale, changes, memory);
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

[[nodiscard]] CacheSlot JitExecutor::Implementation::replacement(
    unsigned long generation, std::uint64_t changes, std::uint64_t pc) {
  const auto set_index = cache_index(generation, pc, geometry_.cache_sets);
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
      .fallback_guest_words =
          &fallback_saved_guest_words_[set_index].ways[selected_way],
  };
}

CompiledBlock &JitExecutor::Implementation::store_native(unsigned long generation, std::uint64_t changes,
                            std::uint64_t pc, JitFunction function,
                            std::uintptr_t internal_entry,
                            JitBlockLink *target_link,
                            std::span<const JitRelocation> relocations,
                            std::size_t direct_link_refusals,
                            std::span<const std::uint32_t> guest_words,
                            std::shared_ptr<const std::uint32_t[]>
                                saved_guest_words,
                            std::uint64_t memory_write_mask,
                            std::uint64_t direct_memory_write_mask,
                            bool accesses_fp_state, bool link_target_safe,
                            bool native_fp, bool native_fpmr,
                            bool guest_illegal
) {
#if !UML_JIT_RUNTIME_DIAGNOSTICS
  (void)direct_link_refusals;
#endif
  /* Native FP/FPMR state always implies materialization. The executor already
   * validates compiler metadata before installation; keep this boundary safe
   * as well if a future caller omits that validation. */
  accesses_fp_state = accesses_fp_state || native_fp || native_fpmr;
  const auto selected = replacement(generation, changes, pc);
  auto &block = *selected.block;
#if UML_JIT_RUNTIME_DIAGNOSTICS
  const bool shared_validation_words = saved_guest_words != nullptr;
#endif
  if (saved_guest_words == nullptr) {
    std::copy(guest_words.begin(), guest_words.end(),
              selected.fallback_guest_words->begin());
    saved_guest_words = std::shared_ptr<const std::uint32_t[]>{
        fallback_saved_guest_words_, selected.fallback_guest_words->data()};
  }
  block.function = function;
  block.internal_entry = internal_entry;
  block.link = target_link;
#if UML_ENABLE_JIT_PROFILE
  profile_jit_target_link_refusal(block.link == nullptr);
  profile_jit_link_table_state(links_.size(), links_.capacity(),
                               indirect_links_.size(),
                               indirect_links_.capacity());
#endif
#if UML_JIT_RUNTIME_DIAGNOSTICS
  cache_validation_diagnostics_.target_link_refusals +=
      block.link == nullptr;
  cache_validation_diagnostics_.direct_link_refusals +=
      direct_link_refusals;
  if (links_.size() >
      cache_validation_diagnostics_.direct_link_table_high_water)
    cache_validation_diagnostics_.direct_link_table_high_water =
        links_.size();
#endif
  block.instruction_count = guest_words.size();
  block.saved_guest_words = std::move(saved_guest_words);
  block.memory_write_mask = memory_write_mask;
  block.direct_memory_write_mask = direct_memory_write_mask;
  block.accesses_fp_state = accesses_fp_state;
  block.link_target_safe = link_target_safe;
  block.native_fp = native_fp;
  block.native_fpmr = native_fpmr;
  block.guest_illegal = guest_illegal;
#if UML_JIT_RUNTIME_DIAGNOSTICS
  ++cache_validation_diagnostics_.translations;
  if (shared_validation_words)
    ++cache_validation_diagnostics_.shared_validation_word_installs;
  else
    ++cache_validation_diagnostics_.private_validation_word_installs;
#endif
  ++compiled_in_link_sequence_;
  for (const auto &relocation : relocations)
    if (relocation.kind == JitRelocationKind::direct_link)
      refresh_cached_link_target(relocation.key);
  activate_link(block);
  return block;
}

void JitExecutor::Implementation::clear_cache() noexcept {
  for (auto &set : *block_cache_)
    set = {};
  const auto generation = link_generation_;
  const auto changes = link_code_changes_;
  const auto initialized = link_sequence_initialized_;
  for (auto &context : link_contexts_) {
    link_context_ = &context;
    deactivate_indirect_links();
    discard_links();
  }
  link_contexts_ = {};
  link_context_ = nullptr;
  links_.clear();
  indirect_links_.clear();
  pending_indirect_ = {};
  tracked_store_arena_.reset();
  link_generation_ = 0;
  link_code_changes_ = 0;
  compiled_in_link_sequence_ = 0;
  link_sequence_initialized_ = false;
  if (initialized)
    prepare_links(generation, changes);
}


[[nodiscard]] SharedTranslationCache::CodegenProfile
JitExecutor::Implementation::shared_codegen_profile() const noexcept {
  return {.capabilities = host_capabilities_,
          .guarded_control_flow = enable_guarded_control_flow_,
          .pointer_authentication = enable_pointer_authentication_,
          .persistent_linked_block_abi =
              persistent_linked_block_abi_default};
}

[[nodiscard]] std::optional<SharedTranslationMaterializationFailure>
JitExecutor::Implementation::materialize_shared_translation(
    const SharedTranslation &translation) {
  if (translation.relocations.size() >
      maximum_shared_translation_relocations)
    return SharedTranslationMaterializationFailure::invalid_metadata;
  relocated_link_pointers_.clear();
  for (const auto &relocation : translation.relocations) {
    const void *pointer = nullptr;
    switch (relocation.kind) {
    case JitRelocationKind::direct_link: {
      /* The arena resolves the target in the private writable link table
       * before patching the literal and publishing executable code.
       */
      pointer = nullptr;
      break;
    }
    case JitRelocationKind::indirect_link:
      pointer = indirect_link_for(relocation.key);
      if (pointer == nullptr)
        return SharedTranslationMaterializationFailure::indirect_link_capacity;
      break;
    default:
      return SharedTranslationMaterializationFailure::invalid_metadata;
    }
    /* Only indirect sites require preallocated executor state here. Direct
     * state is resolved during code publication, with a bounded refusal
     * retaining the unlinked fallback when the table is full.
     */
    relocated_link_pointers_.push_back(
        reinterpret_cast<std::uintptr_t>(pointer));
  }
  return std::nullopt;
}

[[nodiscard]] CompiledBlock *
JitExecutor::Implementation::install_shared_translation(
    unsigned long generation, std::uint64_t changes, std::uint64_t pc,
    std::span<const std::uint32_t> guest_words) {
  if constexpr (!share_translations_between_executors)
    return nullptr;
#if UML_ENABLE_JIT_PROFILE
  const auto materialization_started = jit_profile_timestamp_ns();
#endif
  auto &cache = SharedTranslationCache::instance();
  const auto translation =
      cache.find(shared_codegen_profile(), pc, guest_words);
  if (translation == nullptr)
    return nullptr;
  if (!valid_shared_translation(*translation, guest_words)) {
    cache.record_materialization_failure(
        SharedTranslationMaterializationFailure::invalid_metadata);
    return nullptr;
  }
  if (const auto failure = materialize_shared_translation(*translation)) {
    cache.record_materialization_failure(*failure);
    return nullptr;
  }
  const auto stored = arena_.store_block(
      pc, translation->words, translation->relocations,
      relocated_link_pointers_, links_, empty_link_, generation);
  if (!stored) {
    cache.record_materialization_failure(
        SharedTranslationMaterializationFailure::arena_exhaustion);
    return nullptr;
  }
  const auto internal_entry =
      reinterpret_cast<std::uintptr_t>(stored.code) +
      translation->internal_entry_word * sizeof(std::uint32_t);
  auto *block = &store_native(
      generation, changes, pc,
      sign_jit_function<JitFunction>(stored.code), internal_entry,
      stored.target_link, translation->relocations,
      stored.direct_link_refusals,
      guest_words.first(translation->guest_instructions),
      translation->guest_words,
      translation->memory_write_mask, translation->direct_memory_write_mask,
      translation->accesses_fp_state, translation->link_target_safe,
      translation->native_fp, translation->native_fpmr,
      translation->guest_illegal
  );
  cache.record_reuse();
#if UML_ENABLE_JIT_PROFILE
  profile_jit_shared_materialization_latency(
      jit_profile_timestamp_ns() - materialization_started);
#endif
  return block;
}

std::shared_ptr<const SharedTranslation>
JitExecutor::Implementation::publish_shared_translation(
    std::uint64_t pc, std::span<const std::uint32_t> guest_words,
    const JitCompiler::Result &compiled) {
  if constexpr (!share_translations_between_executors)
    return nullptr;
  auto &cache = SharedTranslationCache::instance();
  if (!compiled.relocatable)
    return nullptr;
  const auto validation_words =
      guest_words.first(compiled.guest_instructions);
  const auto profile = shared_codegen_profile();
  const auto preflight = cache.preflight_publication(
      profile, pc, validation_words,
      {.host_words = compiled.words.size(),
       .relocations = compiled.relocations.size(),
       .guest_words = validation_words.size()},
      compiled.direct_link_refusals,
      compiled.indirect_link_refusals);
  if (preflight.canonical != nullptr)
    return preflight.canonical;
  if (!preflight.may_construct)
    return nullptr;
#if UML_JIT_RUNTIME_DIAGNOSTICS
  if (before_shared_publication_ != nullptr)
    before_shared_publication_(shared_publication_context_);
#endif
  try {
    auto translation = std::make_shared<SharedTranslation>();
    auto saved_guest_words = std::make_shared_for_overwrite<std::uint32_t[]>(
        validation_words.size());
    std::copy(validation_words.begin(), validation_words.end(),
              saved_guest_words.get());
    translation->words.assign(compiled.words.begin(), compiled.words.end());
    translation->relocations.assign(compiled.relocations.begin(),
                                    compiled.relocations.end());
    /* The compiler's literal cells name its executor-local link records.
     * They are never canonical state: every installer resolves the neutral
     * relocation key and resolves its own mutable link records. Keep
     * the shared template free of publisher addresses so identical source and
     * codegen inputs retain identical canonical bytes.
     */
    for (const auto &relocation : translation->relocations) {
      if (relocation.word >= translation->words.size() ||
          translation->words.size() - relocation.word < 2U)
        return nullptr;
      translation->words[relocation.word] = 0;
      translation->words[relocation.word + 1U] = 0;
    }
    translation->direct_link_refusals = compiled.direct_link_refusals;
    translation->indirect_link_refusals = compiled.indirect_link_refusals;
    translation->guest_words = std::move(saved_guest_words);
    translation->guest_word_count = validation_words.size();
    translation->guest_instructions = compiled.guest_instructions;
    translation->hot_words = compiled.hot_words;
    translation->internal_entry_word = compiled.internal_entry_word;
    translation->memory_write_mask = compiled.memory_write_mask;
    translation->direct_memory_write_mask = compiled.direct_memory_write_mask;
    translation->accesses_fp_state = compiled.accesses_fp_state;
    translation->link_target_safe = compiled.link_target_safe;
    translation->native_fp = compiled.native_fp;
    translation->native_fpmr = compiled.native_fpmr;
    translation->guest_illegal = compiled.guest_illegal;
    return cache.insert(profile, pc, validation_words, std::move(translation));
  } catch (const std::bad_alloc &) {
    /* Cross-executor publication is opportunistic. The already-stored native
     * code remains valid and store_native() owns a preallocated private
     * validation-word fallback, so allocation pressure must not become a
     * guest-fatal engine exception.
     */
    return nullptr;
  }
}

} // namespace uml::cpu::arm64
