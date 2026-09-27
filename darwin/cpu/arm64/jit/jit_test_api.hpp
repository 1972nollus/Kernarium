// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/host_capabilities.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace uml::cpu::arm64::jit::testing {

struct CompiledCode final {
  std::vector<std::uint32_t> words;
  std::size_t hot_words{};
  std::size_t direct_link_refusals{};
  std::size_t indirect_link_refusals{};
  std::size_t guest_instructions{};
  std::size_t internal_entry_word{};
  std::uint64_t memory_write_mask{};
  std::uint64_t direct_memory_write_mask{};
  bool accesses_fp_state{};
  bool link_target_safe{};
  bool native_fp{};
  bool native_fpmr{};
  bool guest_illegal{};
  bool gpr_residency_planned{};
  std::size_t gpr_estimated_avoided_accesses{};
};

[[nodiscard]] CompiledCode
compile_block(std::uint64_t pc,
              std::span<const std::uint32_t> instructions);

[[nodiscard]] CompiledCode
compile_block_without_table_list_planning(
    std::uint64_t pc, std::span<const std::uint32_t> instructions);

[[nodiscard]] CompiledCode
compile_block_without_gpr_residency_planning(
    std::uint64_t pc, std::span<const std::uint32_t> instructions);

[[nodiscard]] CompiledCode
compile_block_with_feature_override(
    std::uint64_t pc, std::span<const std::uint32_t> instructions,
    ::uml::cpu::ArmFeature feature, bool available);

[[nodiscard]] CompiledCode
compile_tracked_block(std::uint64_t pc,
                      std::span<const std::uint32_t> instructions);

[[nodiscard]] CompiledCode
compile_linked_block(std::uint64_t pc,
                     std::span<const std::uint32_t> instructions);

[[nodiscard]] CompiledCode
compile_linked_block_with_capacity(
    std::uint64_t pc, std::span<const std::uint32_t> instructions,
    std::size_t capacity);

[[nodiscard]] CompiledCode
compile_indirect_linked_block(
    std::uint64_t pc, std::span<const std::uint32_t> instructions);

[[nodiscard]] CompiledCode
compile_indirect_linked_block_with_capacity(
    std::uint64_t pc, std::span<const std::uint32_t> instructions,
    std::size_t capacity);

[[nodiscard]] std::vector<std::uint32_t>
compile_stable_pointer_load_for_testing(std::uintptr_t value,
                                        std::size_t literal_range_words);

[[nodiscard]] std::vector<std::uint32_t>
compile_two_stable_pointer_loads_for_testing(
    std::uintptr_t first, std::uintptr_t second,
    std::size_t literal_range_words);

[[nodiscard]] std::vector<std::uint32_t>
materialize_constant_for_testing(unsigned destination, std::uint64_t value);

[[nodiscard]] std::vector<std::uint32_t>
lower_memory_barrier_for_testing(std::uint32_t instruction, bool native_sb,
                                 bool native_xs);

[[nodiscard]] std::size_t successful_link_host_word_savings() noexcept;
[[nodiscard]] std::size_t link_chain_instruction_limit() noexcept;
[[nodiscard]] std::size_t link_capability_selector_count() noexcept;
[[nodiscard]] std::size_t block_link_record_size() noexcept;
[[nodiscard]] std::size_t block_link_record_alignment() noexcept;
[[nodiscard]] std::size_t block_link_internal_entry_offset() noexcept;
[[nodiscard]] std::size_t indirect_link_entry_size() noexcept;
[[nodiscard]] std::uint8_t
link_selector_for_capability_mask(std::uint8_t capabilities) noexcept;
[[nodiscard]] std::uint8_t
checked_link_selector_for_capability_mask(std::uint8_t capabilities) noexcept;
[[nodiscard]] std::uint8_t
link_capability_mask_for_selector(std::size_t selector) noexcept;

[[nodiscard]] bool invalid_emitter_metadata_is_rejected_for_testing();

[[nodiscard]] bool
block_link_table_is_bounded(std::size_t capacity);
[[nodiscard]] bool
indirect_link_table_is_bounded(std::size_t capacity);

[[nodiscard]] bool
exclusive_operations_are_serialized_for_testing();
[[nodiscard]] bool
normal_store_leaves_exclusive_gate_unchanged_for_testing();
[[nodiscard]] bool
store_first_publication_handshake_for_testing();
[[nodiscard]] bool
waiting_store_precedes_later_helper_for_testing();
[[nodiscard]] bool
waiting_store_precedes_already_queued_helper_for_testing();
[[nodiscard]] bool
all_waiting_stores_precede_queued_helper_for_testing();
[[nodiscard]] bool
late_store_waits_for_current_helper_for_testing();
[[nodiscard]] bool
tracked_store_invalidation_does_not_deadlock_for_testing();
[[nodiscard]] bool
publication_gate_randomized_stress_for_testing();
[[nodiscard]] bool
publication_participant_lifetime_is_idempotent_for_testing();

} // namespace uml::cpu::arm64::jit::testing
