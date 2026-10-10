// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/jit/jit_compiler.hpp"
#include "cpu/arm64/jit/jit_compiler_families.hpp"
#include "a32_operations.hpp"
#include "a32_transfer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace uml::cpu::arm64::jit::a32 {

/* What the compiler is asked to translate: the guest bytes at the key's
 * fetch address (little endian, as the guest memory holds them), as many as
 * the executor could read. */
struct Request final {
  std::uint64_t key{};
  std::span<const std::byte> bytes;
  /* ITSTATE the entry holds. Nonzero only for a single-instruction
   * translation, which every other translation starts outside an IT block
   * (aarch32/guest_key.hpp). */
  aarch32::ItState itstate{};
  /* Translate exactly the first instruction, which stores the ITSTATE that
   * follows it instead of leaving it to the next block. */
  bool single{};
};

struct Result final {
  JitCompiler::Result translation;
  /* The guest units the translation consumed: words for A32, halfwords for
   * T32 (aarch32::isa_unit_bytes of the key's ISA). The translation's
   * guest_instructions counts instructions. */
  std::size_t guest_units{};
};

/* The AArch32 compiler of the JIT (design 5): a block of A32 or T32
 * instructions decoded by the shared decoder and lowered onto the same
 * residency, flag state and linked exits as the A64 compiler, with the guest
 * PC in key form. R0-R14 are CpuState registers[0..14], zero-extended; all
 * integer arithmetic is on W registers, so every result keeps that form. */
class A32Compiler final : private JitCompilerFamilies {
public:
  using JitCompilerFamilies::IndirectLinkResolver;
  using JitCompilerFamilies::LinkResolver;

  explicit A32Compiler(void *link_context = nullptr,
                       LinkResolver link_resolver = nullptr,
                       bool track_direct_stores = false,
                       IndirectLinkResolver indirect_link_resolver =
                           nullptr) noexcept;

  [[nodiscard]] Result compile(const Request &request);

private:
  using Instruction = aarch32::Instruction;
  using Form = aarch32::Form;
  using Isa = aarch32::Isa;

  struct Decoded final {
    Instruction inst;
    /* The instruction's AArch32 address. */
    std::uint32_t pc{};
    /* ITSTATE before it. */
    std::uint8_t it{};
  };

  enum class Flow : std::uint8_t {
    next,
    /* The instruction ended the block: a branch, an exit, a helper call. */
    end,
    /* The block ends before the instruction, which is not part of it. */
    split,
  };

  struct ColdPath final {
    enum class Kind : std::uint8_t {
      /* The access faulted: report it at the instruction. */
      memory_fault,
      /* The memory instruction the hot path declined (unaligned,
       * crossing a page, or in need of a helper): the C++ helper runs it. */
      memory_slow,
      /* The exclusive the hot path declined: the C++ helper runs it. */
      exclusive_slow,
      /* Retry exit of a fused exclusive region. */
      exclusive_mismatch,
      /* The instruction is UNDEFINED at run time (a CP15 barrier while
       * SCTLR.CP15BEN is clear). */
      illegal,
    };
    Arm64Emitter::Label label;
    Kind kind;
    std::size_t index;
    std::uint16_t dirty_gprs{};
    std::uint32_t dirty_vectors{};
    GuestFlagsState flags{GuestFlagsState::memory};
  };

  struct TranslationColdPath final {
    Arm64Emitter::Label label;
    Arm64Emitter::Label continuation;
    Arm64Emitter::Label fault;
    unsigned address;
    ::uml::memory::Access access;
  };

  /* A host register holding a W value and, for a destination, how it is
   * committed. */
  struct Dest final {
    NativeGpr native;
    unsigned guest;
  };

  /* The register bindings a conditional body must leave as it found them. */
  struct ResidencyMark final {
    decltype(JitCompilerFamilies::gpr_slots_) gpr;
    decltype(JitCompilerFamilies::vector_slots_) vector;
  };
  /* How a logical operation sets the carry flag. */
  enum class CarryKind : std::uint8_t {
    /* C keeps its value. */
    unchanged,
    clear,
    set,
    /* C is the bit of carry_reg selected by a rotation (RMIF). */
    bit,
    /* C is bit 1 of x0, from a register-controlled shift. */
    computed,
  };

  // a32_compiler.cpp: the driver.
  void decode_block(const Request &request);
  void emit_block();
  Flow emit_instruction();
  Flow emit_guarded(unsigned condition);
  [[nodiscard]] Flow emit_operation();
  void emit_cold_paths();
  void emit_slow_helper(const ColdPath &cold);
  void emit_deferred_exclusive(const ColdPath &cold);
  [[nodiscard]] ResidencyMark mark_residency() const;
  void restore_residency(const ResidencyMark &mark);
  void close_conditional_body(const ResidencyMark &mark);

  // Exits.
  [[nodiscard]] std::uint64_t key_at(std::uint32_t pc) const noexcept;
  [[nodiscard]] std::uint64_t key_of(const Decoded &d) const noexcept {
    return key_at(d.pc);
  }
  [[nodiscard]] std::uint64_t next_key() const noexcept {
    return key_at(cur().pc + cur().inst.length);
  }
  void store_it(std::uint8_t value);
  void exit_event(Event event, std::uint64_t pc_key, std::uint8_t it);
  void exit_illegal();
  void exit_direct(std::uint64_t target_key);
  void exit_indirect(unsigned key_register);
  void exit_continue_at(std::uint64_t pc_key, std::uint8_t it,
                        std::size_t count);
  [[nodiscard]] Arm64Emitter::Label cold_exit(ColdPath::Kind kind);

  // Registers and constants (a32_compiler_alu.cpp).
  [[nodiscard]] const Decoded &cur() const noexcept { return block_[count_]; }
  [[nodiscard]] std::uint32_t pc_read() const noexcept;
  [[nodiscard]] std::uint32_t pc_align_read() const noexcept {
    return pc_read() & ~3U;
  }
  /* The host register holding guest register r (the PC's value for r15). */
  [[nodiscard]] unsigned use(unsigned r, unsigned fallback);
  [[nodiscard]] Dest def(unsigned r, bool reads_old, unsigned fallback);
  void done(const Dest &dest);
  void load_u32(unsigned destination, std::uint32_t value);
  void clobber_flags();

  // Data processing (a32_compiler_alu.cpp).
  [[nodiscard]] Flow emit_data_processing(const OpInfo &info);
  [[nodiscard]] Flow emit_address_forms(const OpInfo &info);
  [[nodiscard]] unsigned shift_value(unsigned source, aarch32::SRType type,
                                     unsigned amount, unsigned scratch);
  void shift_by_register(unsigned out, unsigned rm, unsigned rs,
                         aarch32::SRType type);
  void shift_by_register_carry(unsigned out, unsigned rm, unsigned rs,
                               aarch32::SRType type);
  void restore_logical_flags(CarryKind carry, unsigned carry_reg,
                             unsigned carry_rot);

  // Memory (a32_compiler_memory.cpp).
  [[nodiscard]] Flow emit_memory(const OpInfo &info);
  [[nodiscard]] Flow emit_single(const Transfer &transfer);
  [[nodiscard]] Flow emit_pair(const Transfer &transfer);
  [[nodiscard]] Flow emit_multiple(const Transfer &transfer);
  [[nodiscard]] Flow emit_table(const Transfer &transfer);
  [[nodiscard]] Flow emit_exclusive(const Transfer &transfer);
  /* The whole instruction in the slow helper. */
  [[nodiscard]] Flow emit_through_helper();
  void emit_translate(unsigned address, ::uml::memory::Access access,
                      Arm64Emitter::Label slow, Arm64Emitter::Label fault,
                      std::size_t bytes);
  void emit_translation_cold(const TranslationColdPath &path);
  void emit_alignment_check(unsigned address, unsigned alignment,
                            Arm64Emitter::Label slow);
  /* The address of a single or paired transfer in x8 (the base for a
   * post-indexed one). */
  void form_address(const Transfer &transfer);
  /* The base register after a writeback transfer, from the registers as
   * they stand (they hold the values the address used). */
  void write_back(const Transfer &transfer);
  void mark_memory_write() noexcept;
  void mark_direct_memory_write() noexcept;
  void track_direct_store(unsigned address, std::size_t size);
  void clear_exclusive_monitor();

  // Control (a32_compiler_control.cpp).
  [[nodiscard]] Flow emit_branch(const OpInfo &info);
  [[nodiscard]] Flow emit_status_register(const OpInfo &info);
  [[nodiscard]] Flow emit_coprocessor();
  [[nodiscard]] Flow emit_other(const OpInfo &info);
  void build_interwork_key(unsigned value);
  void build_thumb_key(unsigned value);
  void write_pc(unsigned value, bool interwork);
  void write_pc(unsigned value) { write_pc(value, !thumb_); }
  [[nodiscard]] std::uint32_t link_value() const noexcept;
  [[nodiscard]] bool barrier_option_is_store() const noexcept;

  // Per-block state.
  Arm64Emitter emitter_;
  std::vector<Decoded> block_;
  std::vector<ColdPath> cold_;
  std::vector<TranslationColdPath> translation_cold_;
  std::size_t count_{};
  std::size_t units_consumed_{};
  std::uint64_t key_{};
  Isa isa_{Isa::a32};
  bool thumb_{};
  bool single_{};
  bool track_direct_stores_{};
  bool terminated_{};
  bool native_fp_{};
  bool link_target_safe_{true};
  bool guest_illegal_{};
  std::uint64_t memory_write_mask_{};
  std::uint64_t direct_memory_write_mask_{};
  aarch32::Profile profile_;
};

} // namespace uml::cpu::arm64::jit::a32
