// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/jit/jit_compiler.hpp"
#include "cpu/arm64/jit/jit_compiler_families.hpp"
#include "a32_operations.hpp"
#include "a32_transfer.hpp"
#include "a32_vector_operations.hpp"

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
      /* The instruction the hot path declined (a memory access that is
       * unaligned, crosses a page or needs the helper, or a form with no
       * inline lowering): the C++ helper runs it. */
      slow,
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

  /* The skipped arm of a fused conditional store: it clears the monitor and
   * resumes after the store. */
  struct ResumePath final {
    Arm64Emitter::Label label;
    Arm64Emitter::Label continuation;
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
  /* A static successor: linked when the ITSTATE that follows is zero. The
   * one-argument form is the exit of the instruction being translated. */
  void exit_direct(std::uint64_t target_key);
  void exit_direct(std::uint64_t target_key, std::uint8_t it,
                   std::size_t count);
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
  /* LL/SC fusion (a32_compiler_exclusive.cpp): the number of instructions of
   * the block's start it lowered to a host load-exclusive/store-exclusive
   * run, or 0 when it declined. */
  [[nodiscard]] std::size_t try_fuse_exclusive();
  void clear_exclusive_monitor();

  // Multiplies, bit manipulation, extension, saturation and DSP forms
  // (a32_compiler_arith.cpp).
  [[nodiscard]] Flow emit_multiply(const OpInfo &info);
  [[nodiscard]] Flow emit_bit_operation(const OpInfo &info);
  [[nodiscard]] Flow emit_extend(const OpInfo &info);
  [[nodiscard]] Flow emit_saturate(const OpInfo &info);
  [[nodiscard]] Flow emit_dsp_multiply(const OpInfo &info);
  /* The whole instruction in the slow helper. */
  [[nodiscard]] Flow emit_arith_through_helper();
  void emit_extract(unsigned out, unsigned source, bool is_signed,
                    unsigned bits, unsigned rotation);
  void emit_half(unsigned out, unsigned source, unsigned index);
  void or_q(unsigned flag);
  void saturating_add(unsigned out, unsigned a, unsigned b, bool subtract,
                      bool accumulate);
  void set_q_on_overflow();

  // Vectors: the register views and the floating-point environment
  // (a32_compiler_vector.cpp).
  [[nodiscard]] Flow emit_vector(const OpInfo &info);
  /* Q n is CpuState vector n: its host register holds the whole register. */
  [[nodiscard]] unsigned q_use(unsigned q, unsigned fallback);
  [[nodiscard]] NativeVector q_def(unsigned q, bool reads_old,
                                   unsigned fallback);
  void q_done(NativeVector value, unsigned q);
  /* D n is the low (even n) or high (odd n) half of Q n/2. The returned
   * host register holds the doubleword in its low 64 bits (the rest is
   * unspecified), staged in `scratch` when it is not already there. */
  [[nodiscard]] unsigned d_use(unsigned d, unsigned scratch);
  /* The doubleword in both halves of the returned register. */
  [[nodiscard]] unsigned d_use_twice(unsigned d, unsigned scratch);
  /* D n takes the low 64 bits of `result`; the other half is kept. */
  void d_write(unsigned d, unsigned result);
  /* S n is lane n%4 of Q n/4: the returned register holds it in its low 32
   * bits. */
  [[nodiscard]] unsigned s_use(unsigned s, unsigned scratch);
  void s_write(unsigned s, unsigned result);
  /* A VFP operand: a single or a double register. */
  [[nodiscard]] unsigned fp_use(unsigned r, bool dp, unsigned scratch) {
    return dp ? d_use(r, scratch) : s_use(r, scratch);
  }
  void fp_write(unsigned r, bool dp, unsigned result) {
    if (dp)
      d_write(r, result);
    else
      s_write(r, result);
  }
  /* The instruction reads or writes vector or floating-point state. */
  void touch_vectors() noexcept { accesses_fp_state_ = true; }
  /* The instruction depends on the host FPCR or FPSR, or sets FPSR. */
  void touch_fp_environment();
  /* Moves the host FPCR to the guest's value or to the Standard FPSCR
   * value (FZ and DN set, round to nearest, AHP and FZ16 the guest's), when
   * it is not there already: the translation knows the mode only after an
   * instruction of its own, and tests CpuState::a32_fp_standard for the
   * first. */
  void set_fp_mode(bool standard);
  [[nodiscard]] std::uint32_t neon_word() const noexcept;

  // Vector memory (a32_compiler_vmem.cpp).
  [[nodiscard]] Flow emit_vector_memory(const VInfo &v);
  [[nodiscard]] Flow emit_vector_register_transfer(bool store);
  [[nodiscard]] Flow emit_vector_block_transfer(bool store);
  [[nodiscard]] Flow emit_vector_structure(const VInfo &v);
  [[nodiscard]] Flow vmem_through_helper(bool store);
  void vmem_granule(bool load, unsigned log2_size, unsigned index,
                    unsigned offset);
  void vmem_run(bool load, bool single, unsigned first, unsigned count);
  void vmem_d_from(unsigned dreg, unsigned source, unsigned half);
  void vmem_d_into(unsigned dreg, unsigned scratch, unsigned half);

  // The Armv8 extensions (a32_compiler_armv8.cpp).
  [[nodiscard]] Flow emit_extension(const OpInfo &info);
  [[nodiscard]] Flow emit_crc32();
  [[nodiscard]] Flow emit_crypto();
  [[nodiscard]] Flow emit_vsel();
  [[nodiscard]] Flow emit_vmaxnm();
  [[nodiscard]] Flow emit_vrint();
  [[nodiscard]] Flow emit_vcvt_round();
  [[nodiscard]] Flow emit_vjcvt();
  [[nodiscard]] Flow emit_dot(bool by_element);
  [[nodiscard]] Flow emit_rdm();
  [[nodiscard]] Flow emit_vcadd();
  [[nodiscard]] Flow emit_vcmla(bool by_element);

  // VFP (a32_compiler_vfp.cpp).
  [[nodiscard]] Flow emit_vfp(const OpInfo &info, const VInfo &v);
  [[nodiscard]] Flow emit_vfp_arith(const VInfo &v);
  [[nodiscard]] Flow emit_vfp_compare(const VInfo &v);
  [[nodiscard]] Flow emit_vfp_convert(const VInfo &v);
  [[nodiscard]] Flow emit_vfp_move(const VInfo &v);
  [[nodiscard]] Flow emit_fpscr(const VInfo &v);

  // Advanced SIMD (a32_compiler_neon*.cpp).
  [[nodiscard]] Flow emit_neon(const VInfo &v);
  [[nodiscard]] bool neon_half_precision(const VInfo &v) const;
  [[nodiscard]] Flow emit_neon_same(const VInfo &v);
  [[nodiscard]] Flow emit_neon_step(bool rsqrt, bool quad, unsigned d,
                                    unsigned n, unsigned m);
  [[nodiscard]] Flow emit_neon_logic(const VInfo &v);
  [[nodiscard]] Flow emit_neon_misc(const VInfo &v);
  [[nodiscard]] Flow emit_neon_convert(const VInfo &v);
  [[nodiscard]] Flow emit_neon_immediate(const VInfo &v);
  [[nodiscard]] Flow emit_neon_permute(const VInfo &v);
  [[nodiscard]] Flow emit_neon_table(const VInfo &v);
  [[nodiscard]] Flow emit_neon_shift(const VInfo &v);
  [[nodiscard]] Flow emit_neon_diff(const VInfo &v);
  [[nodiscard]] Flow emit_neon_scalar(const VInfo &v);
  [[nodiscard]] static unsigned esize_log2(int esize) noexcept;
  /* An A64 vector instruction with its register fields clear, given them. */
  void emit_vec3(std::uint32_t base, unsigned rd, unsigned rn, unsigned rm) {
    emit(base | rd | (rn << 5) | (rm << 16));
  }
  void emit_vec2(std::uint32_t base, unsigned rd, unsigned rn) {
    emit(base | rd | (rn << 5));
  }
  /* Register copy (a no-op when they are the same register). */
  void vmov(unsigned d, unsigned n);
  /* The register or half register `reg` (a D or Q number) as a source. */
  [[nodiscard]] unsigned vsrc(unsigned reg, bool quad, unsigned scratch);
  void neon_op3(std::uint32_t base, bool quad, bool wide, unsigned d,
                unsigned n, unsigned m, bool reads_dest);
  void neon_op2(std::uint32_t base, bool quad, bool wide, unsigned d,
                unsigned m, bool reads_dest);
  void q_put(unsigned q, unsigned result);
  void neon_put(unsigned reg, bool quad, unsigned result);

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
  std::vector<ResumePath> resume_cold_;
  std::size_t count_{};
  std::size_t units_consumed_{};
  std::uint64_t key_{};
  Isa isa_{Isa::a32};
  bool thumb_{};
  bool single_{};
  bool track_direct_stores_{};
  bool terminated_{};
  bool native_fp_{};
  bool accesses_fp_state_{};
  /* The mode of the host FPCR where the instruction being translated
   * starts, as far as the translation can tell: it is unknown where the
   * translation begins and where two arms of a conditional body differ. */
  enum class FpMode : std::uint8_t { unknown, guest, standard };
  FpMode fp_mode_{FpMode::unknown};
  bool link_target_safe_{true};
  bool guest_illegal_{};
  std::uint64_t memory_write_mask_{};
  std::uint64_t direct_memory_write_mask_{};
  aarch32::Profile profile_;
};

} // namespace uml::cpu::arm64::jit::a32
