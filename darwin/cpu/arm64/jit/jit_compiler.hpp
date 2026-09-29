// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_compiler_families.hpp"
#include "jit_emitter.hpp"
#include "jit_runtime_contract.hpp"
#include "jit_semantic_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace uml::cpu::arm64::detail {
struct AtomicFloating;
}

namespace uml::cpu::arm64 {

using ::uml::memory::Access;

class JitCompiler final : private jit::JitCompilerFamilies {
public:
  explicit JitCompiler(void *link_context = nullptr,
                       LinkResolver link_resolver = nullptr,
                       bool track_direct_stores = false,
                       IndirectLinkResolver indirect_link_resolver =
                           nullptr) noexcept;

  explicit JitCompiler(const ArmFeatureSet &translation_features) noexcept;

  void set_table_list_planning_enabled(bool enabled) noexcept;

  struct Result final {
    std::span<const std::uint32_t> words;
    /* Where this translation emitted executor-owned link operands. Shared
     * publication zeros those cells and retains these neutral guest keys so
     * another executor can resolve its own direct-link records and PIC sites.
     * Only a fully recorded translation may be reused.
     */
    std::span<const JitRelocation> relocations;
    bool relocatable{};
    /* Configured link resolvers can refuse stable direct or indirect records
     * when their bounded tables are full. Such code remains correct through
     * the ordinary C++ fallback, but is a degraded private result rather than
     * a canonical cross-executor translation.
     */
    std::size_t direct_link_refusals{};
    std::size_t indirect_link_refusals{};
    std::size_t guest_instructions{};
    std::size_t hot_words{};
    std::size_t internal_entry_word{};
    std::uint64_t memory_write_mask{};
    std::uint64_t direct_memory_write_mask{};
    bool accesses_fp_state{};
    bool link_target_safe{true};
    bool native_fp{};
    bool native_fpmr{};
    bool guest_illegal{};
  };

  Result compile(std::uint64_t guest_pc,
                 std::span<const std::uint32_t> instructions);

  Result compile_once(std::uint64_t guest_pc,
                      std::span<const std::uint32_t> instructions);

private:
  enum class Action {
    keep_going,
    terminate,
    unsupported,
  };

  void select_constrained_vector_pins(
      std::span<const std::uint32_t> instructions) noexcept;

  void select_consecutive_vector_lists(
      std::span<const std::uint32_t> instructions) noexcept;

  [[nodiscard]] bool
  translation_supports(ArmFeature feature) const noexcept;

  struct ColdPath final {
    enum class Action {
      memory_fault,
      pair_slow,
      simd_structure_slow,
      alignment_fault,
      ordered_slow,
      exclusive_slow,
      deferred_exclusive_slow,
      exclusive_compare_mismatch,
      memory_slow,
    };

    Arm64Emitter::Label label;
    std::uint32_t instruction;
    std::uint64_t pc;
    std::size_t count;
    Action action;
    std::uint16_t dirty_gprs{};
    std::uint32_t dirty_vectors{};
    GuestFlagsState flags{GuestFlagsState::memory};
  };

  struct TranslationColdPath final {
    Arm64Emitter::Label label;
    Arm64Emitter::Label continuation;
    Arm64Emitter::Label fault;
    unsigned address;
    Access access;
  };

  class ColdPaths final {
  public:
    explicit ColdPaths(JitCompiler *compiler) noexcept
        : compiler_{compiler} {}

    void push_back(ColdPath path) {
      /*
       * Cold labels are emitted after the complete hot block, when later
       * guest instructions may have changed the compiler's flag state. Keep
       * the state at the edge with the label; flag-neutral guards allow the
       * rare arm to publish it without adding work to the hot path.
       */
      path.flags = compiler_->snapshot_flags();
      path.dirty_gprs = compiler_->dirty_gpr_mask();
      path.dirty_vectors = compiler_->dirty_vector_mask();
      paths_.push_back(path);
    }

    [[nodiscard]] std::vector<ColdPath>::const_iterator
    begin() const noexcept {
      return paths_.begin();
    }
    [[nodiscard]] std::vector<ColdPath>::const_iterator
    end() const noexcept {
      return paths_.end();
    }

  private:
    JitCompiler *compiler_;
    std::vector<ColdPath> paths_;
  };

  [[nodiscard]] static bool
  reservation_sensitive_allocation(std::uint32_t instruction) noexcept;

  void mark_memory_write() noexcept;

  void mark_direct_memory_write() noexcept;

  void track_direct_store(unsigned address, std::size_t size);

  void track_direct_store(unsigned address, std::size_t size,
                          std::size_t instruction_index);

  [[nodiscard]] bool emit_primary_hint(std::uint32_t instruction);

  void mask_register(unsigned reg, std::uint64_t mask, bool wide);

  [[nodiscard]] bool emit_system_register(std::uint32_t instruction);

  void emit_cache_zero(std::uint32_t instruction, std::uint64_t pc);

  void emit_code_synchronize(std::uint32_t instruction, std::uint64_t pc,
                             std::size_t count);

  void emit_pair_slow(std::uint32_t instruction, std::uint64_t pc,
                      std::size_t count);

  void emit_simd_structure_slow(std::uint32_t instruction, std::uint64_t pc,
                                std::size_t count);

  void emit_alignment_fault(std::uint32_t instruction, std::uint64_t pc,
                            std::size_t count);

  void emit_ordered_slow(std::uint32_t instruction, std::uint64_t pc,
                         std::size_t count);

  void emit_exclusive_slow(std::uint32_t instruction, std::uint64_t pc,
                           std::size_t count);

  void emit_deferred_exclusive_slow(std::uint32_t instruction,
                                    std::uint64_t pc, std::size_t count);

  void emit_memory_slow(std::uint32_t instruction, std::uint64_t pc,
                        std::size_t count);


  void emit_simd_modified_immediate(std::uint32_t instruction,
                                    bool reads_destination);

  void emit_simd_duplicate_general(std::uint32_t instruction);

  [[nodiscard]] bool emit_simd_insert(std::uint32_t instruction);

  [[nodiscard]] bool emit_simd_duplicate_element(std::uint32_t instruction);

  void emit_simd_unary_native(std::uint32_t instruction);

  [[nodiscard]] bool emit_simd_bit_unary(std::uint32_t instruction);

  [[nodiscard]] bool emit_simd_integer_reduce(std::uint32_t instruction);

  [[nodiscard]] bool emit_simd_scalar_pairwise_add(std::uint32_t instruction);

  [[nodiscard]] bool emit_simd_shift_immediate(std::uint32_t instruction);

  [[nodiscard]] bool emit_simd_shift_left_long(std::uint32_t instruction);

  [[nodiscard]] bool emit_simd_general_move(std::uint32_t instruction);

  void emit_translate(unsigned address, Access access,
                      Arm64Emitter::Label fallback, Arm64Emitter::Label fault,
                      std::size_t bytes);

  void emit_translation_cold(const TranslationColdPath &path);

  void emit_misalignment_check(unsigned address, std::size_t alignment,
                               Arm64Emitter::Label misaligned);

  void clear_local_exclusive_monitor();

  [[nodiscard]] std::size_t
  try_emit_native_exclusive_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc);

  void emit_ls64(std::uint32_t instruction, std::uint64_t pc, bool load,
                 bool status, bool accdata);

  [[nodiscard]] Action emit_exclusive_memory(std::uint32_t instruction,
                                             std::uint64_t pc);

  [[nodiscard]] Action emit_atomic_memory(std::uint32_t instruction,
                                          std::uint64_t pc);

  void emit_native_scalar_atomic(std::uint32_t instruction,
                                 std::uint64_t pc);

  void emit_native_scalar_compare_swap(std::uint32_t instruction,
                                       std::uint64_t pc);

  void emit_atomic_floating_memory(
      std::uint32_t instruction, std::uint64_t pc,
      const arm64::detail::AtomicFloating &decoded);

  [[nodiscard]] bool emit_pair_memory(std::uint32_t instruction,
                                      std::uint64_t pc);

  [[nodiscard]] bool emit_simd_structure_memory(std::uint32_t instruction,
                                                std::uint64_t pc, bool single);

  [[nodiscard]] bool emit_other_memory(std::uint32_t instruction,
                                       std::uint64_t pc);

  [[nodiscard]] bool emit_literal_memory(std::uint32_t instruction,
                                         std::uint64_t pc);

  struct SimdUnsignedMemoryOperation final {
    std::uint32_t instruction{};
    std::uint64_t offset{};
    std::size_t bytes{};
    unsigned base{};
    unsigned transfer{};
    bool load{};
  };

  [[nodiscard]] static bool decode_simd_unsigned_memory(
      std::uint32_t instruction,
      SimdUnsignedMemoryOperation &operation) noexcept;

  [[nodiscard]] std::size_t try_emit_simd_unsigned_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc);

  struct SpUnsignedMemoryOperation final {
    std::uint32_t instruction{};
    std::uint64_t offset{};
    std::size_t bytes{};
    unsigned transfer{};
    bool load{};
  };

  [[nodiscard]] static bool decode_sp_unsigned_memory(
      std::uint32_t instruction,
      SpUnsignedMemoryOperation &operation) noexcept;

  [[nodiscard]] std::size_t try_emit_sp_unsigned_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc);

  struct GprUnsignedMemoryOperation final {
    std::uint32_t instruction{};
    std::uint64_t offset{};
    std::size_t bytes{};
    unsigned base{};
    unsigned transfer{};
    bool load{};
  };

  [[nodiscard]] static bool decode_gpr_unsigned_memory(
      std::uint32_t instruction,
      GprUnsignedMemoryOperation &operation) noexcept;

  [[nodiscard]] std::size_t try_emit_gpr_unsigned_run(
      std::span<const std::uint32_t> instructions, std::uint64_t pc);

  [[nodiscard]] bool emit_unsigned_memory(std::uint32_t instruction,
                                          std::uint64_t pc);

  /*
   * These ordinary control-flow classes terminate almost every translated
   * block and do not overlap the system, ordered-memory, or extension
   * allocations handled below. Resolve them immediately after architectural
   * exception instructions so cold translation does not walk the complete
   * decoder for a branch already known to end the block.
   */
  bool emit_common_control_flow(std::uint32_t instruction, std::uint64_t pc,
                                std::size_t resulting_count);

  Action emit_instruction(std::uint32_t instruction, std::uint64_t pc);

  Arm64Emitter emitter_;
  ColdPaths cold_;
  std::vector<TranslationColdPath> translation_cold_;
  std::size_t count_{};
  std::uint64_t memory_write_mask_{};
  std::uint64_t direct_memory_write_mask_{};
  bool terminated_{};
  bool accesses_fp_state_{};
  bool link_target_safe_{true};
  bool native_fp_{};
  bool native_fpmr_{};
  bool guest_illegal_{};
  void *configured_link_context_{};
  LinkResolver configured_link_resolver_{};
  IndirectLinkResolver configured_indirect_link_resolver_{};
  bool track_direct_stores_{};
  bool table_list_planning_enabled_{true};
  const ArmFeatureSet *translation_features_{};
};

using JitFunction = std::uint64_t (*)(CpuState *, Tlb *, JitChainState *);

} // namespace uml::cpu::arm64
