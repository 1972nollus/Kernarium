// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Non-streaming SVE in the arm64 JIT (design 4.1 and 4.3): the control core
 * (SVE lane V4, milestone M1). An SM=0 translation lowers each SVE word
 * onto NEON and general registers at VL 16 bytes, and never emits one.
 * Z_n is the guest's V_n in the vector bindings; P0-P15 and FFR are
 * 16-byte mask-form images (one 0x00 or 0xff byte per predicate bit) bound
 * beside them (read_native_predicate). Element e of esize bytes is active
 * when byte e * esize of its predicate is 0xff.
 *
 * Host scratch: v0-v3 are the bindings' fallbacks, v4 and v5 intermediate
 * values; x8-x11 and x16 general.
 */
#include "jit_compiler.hpp"
#include "jit_sve_encoders.hpp"
#include "jit_sve_support.hpp"

#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/arm64/sve_profile.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <optional>
#include <utility>

namespace uml::cpu::arm64 {

namespace {

namespace forms = jit::sve_forms;

using namespace jit::encoders;

/* The 64-bit value's MOVI byte-mask immediate, when each byte is 0x00 or
 * 0xff. */
[[nodiscard]] constexpr std::optional<unsigned>
byte_mask_immediate(std::uint64_t value) noexcept {
  unsigned bytes = 0;
  for (unsigned index = 0; index < 8U; ++index) {
    const auto byte = (value >> (index * 8U)) & 0xffU;
    if (byte == 0xffU)
      bytes |= 1U << index;
    else if (byte != 0U)
      return std::nullopt;
  }
  return bytes;
}

[[nodiscard]] constexpr std::uint64_t
predicate_half(const SvePredicate &predicate, unsigned half) noexcept {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8U; ++index)
    value |= std::uint64_t{predicate[half * 8U + index]} << (index * 8U);
  return value;
}

/* VFPExpandImm() for a half, single or double. */
[[nodiscard]] constexpr std::uint64_t expand_float_immediate(
    unsigned immediate, unsigned bits) noexcept {
  const unsigned exponent_bits = bits == 16U ? 5U : bits == 32U ? 8U : 11U;
  const unsigned fraction_bits = bits - exponent_bits - 1U;
  const std::uint64_t sign = (immediate >> 7) & 1U;
  const std::uint64_t b = (immediate >> 6) & 1U;
  const std::uint64_t exponent =
      ((b ^ 1U) << (exponent_bits - 1U)) |
      (b != 0U ? ((std::uint64_t{1} << (exponent_bits - 3U)) - 1U) << 2 : 0U) |
      ((immediate >> 4) & 3U);
  return (sign << (bits - 1U)) | (exponent << fraction_bits) |
         (std::uint64_t{immediate & 15U} << (fraction_bits - 4U));
}

static_assert(expand_float_immediate(0x70U, 32U) == 0x3f80'0000U); // 1.0
static_assert(expand_float_immediate(0x00U, 64U) ==
              0x4000'0000'0000'0000ULL); // 2.0
static_assert(expand_float_immediate(0xf0U, 16U) == 0xbc00U); // -1.0

/* One active count's entry of a WHILE* table: the predicate and the NZCV
 * PredTest() gives it. */
struct alignas(32) WhileEntry final {
  SvePredicate predicate;
  std::uint32_t nzcv;
};
static_assert(sizeof(WhileEntry) == 32U);
constexpr unsigned while_entry_shift = 5U;
static_assert(offsetof(WhileEntry, nzcv) == 16U);

/* For each element size, indexed by the active count clamped to 16: the
 * leading count elements active (the incrementing and conflict forms) or
 * the trailing ones (the decrementing forms). A count above the element
 * count makes every element active. */
using WhileTable = std::array<WhileEntry, 17>;

template <bool Trailing>
[[nodiscard]] constexpr WhileTable while_table(unsigned esize) noexcept {
  const unsigned elements = sve_vector_length / esize;
  WhileTable table{};
  for (unsigned count = 0; count < table.size(); ++count) {
    const unsigned active = count < elements ? count : elements;
    auto &entry = table[count];
    if constexpr (!Trailing) {
      const auto leading = sve_leading_predicate(esize, active);
      entry.predicate = leading.predicate;
      entry.nzcv = leading.nzcv;
    } else {
      for (unsigned element = elements - active; element < elements;
           ++element)
        entry.predicate[element * esize] = 0xffU;
      /* N: the first element is active; Z: none is; C: the last is not. */
      entry.nzcv = (active == elements ? 0x8000'0000U : 0U) |
                   (active == 0U ? 0x6000'0000U : 0U);
    }
  }
  return table;
}

template <bool Trailing>
constexpr std::array<WhileTable, 4> while_tables{
    while_table<Trailing>(1U), while_table<Trailing>(2U),
    while_table<Trailing>(4U), while_table<Trailing>(8U)};

static_assert(while_tables<false>[2][3].nzcv == 0xa000'0000U); // 3 of 4
static_assert(while_tables<false>[2][16].nzcv == 0x8000'0000U);
static_assert(while_tables<true>[1][1].predicate[14] == 0xffU &&
              while_tables<true>[1][1].nzcv == 0U);
static_assert(while_tables<true>[3][9].nzcv == 0x8000'0000U);

/* DUP and CPY (immediate): the signed byte, shifted by 8 when sh is set,
 * as an element of esize bytes. */
[[nodiscard]] constexpr std::uint64_t integer_element(std::uint32_t instruction,
                                                      unsigned esize) noexcept {
  const auto value =
      static_cast<std::uint64_t>(sign_extend(field(instruction, 5, 8), 8))
      << (field(instruction, 13, 1) * 8U);
  return esize == 8U ? value : value & ((std::uint64_t{1} << (esize * 8U)) - 1U);
}

[[nodiscard]] constexpr bool while_form(std::uint16_t form) noexcept {
  for (const auto candidate :
       {forms::whilelt, forms::whilele, forms::whilelo, forms::whilels,
        forms::whilegt, forms::whilege, forms::whilehi, forms::whilehs,
        forms::whilewr, forms::whilerw})
    if (form == candidate)
      return true;
  return false;
}

[[nodiscard]] constexpr bool predicate_form(std::uint16_t form) noexcept {
  for (const auto candidate :
       {forms::ptrue, forms::ptrues, forms::pfalse, forms::ptest,
        forms::and_p, forms::ands_p, forms::bic_p, forms::bics_p,
        forms::eor_p, forms::eors_p, forms::orr_p, forms::orrs_p,
        forms::orn_p, forms::orns_p, forms::nand_p, forms::nands_p,
        forms::nor_p, forms::nors_p, forms::sel_p, forms::setffr,
        forms::rdffr, forms::rdffr_predicated, forms::rdffrs, forms::wrffr})
    if (form == candidate)
      return true;
  return false;
}

[[nodiscard]] constexpr bool count_form(std::uint16_t form) noexcept {
  for (const auto candidate :
       {forms::cntp, forms::incp_scalar, forms::decp_scalar,
        forms::incp_vector, forms::decp_vector, forms::cnt, forms::inc_scalar,
        forms::dec_scalar, forms::inc_vector, forms::dec_vector, forms::rdvl,
        forms::addvl, forms::addpl})
    if (form == candidate)
      return true;
  return false;
}

} // namespace

std::optional<JitCompiler::Action>
JitCompiler::emit_sve_instruction(std::uint32_t instruction,
                                  std::uint64_t pc) {
  const auto *row = detail::find_sme_encoding(instruction);
  if (row == nullptr ||
      (row->non_streaming_requirement & detail::sme_feature_sve) == 0U)
    return std::nullopt;
  const auto required = row->non_streaming_requirement;
  if ((required & implemented_sve_features(host_capabilities())) != required ||
      ((row->mode & detail::sme_mode_za) != 0U &&
       (sme_mode() & svcr_za) == 0U))
    return Action::unsupported;
  const auto form = row->form;
  if (jit::sve_word_reserved(*row, instruction))
    return Action::unsupported;
  /* The hot set lowers inline for the all-active predicate; every other
   * row the profile implements, and a word the lowering declines, runs
   * through the semantic library: a terminal helper, as the SME slow paths
   * are. */
  if (!jit::implements_sve_form(form)) {
    if (emit_sve_inline(*row, instruction, pc))
      return Action::keep_going;
    if (row->memory != 0U &&
        detail::sme_encoding_memory_of(*row)->direction ==
            detail::SmeMemoryDirection::store)
      mark_memory_write();
    emit_terminal_helper(
        reinterpret_cast<const void *>(&uml_arm64_jit_execute_sve),
        instruction, pc, count_ + 1U);
    return Action::terminate;
  }
  begin_sve_word();
  if (while_form(form))
    return emit_sve_while(form, instruction, pc) ? Action::terminate
                                                 : Action::keep_going;
  if (predicate_form(form))
    emit_sve_predicate_form(form, instruction);
  else if (count_form(form))
    emit_sve_count_form(form, instruction);
  else
    emit_sve_move_form(form, instruction);
  return Action::keep_going;
}

void JitCompiler::emit_sve_flags_from_w11() {
  emitter_.emit(msr_nzcv_x11);
  store_flags();
}

void JitCompiler::emit_sve_predicate_test(unsigned mask, unsigned result) {
  /*
   * x8 = g and x9 = r, four bits per predicate byte (SHRN #4 of the mask
   * form); a = g & r, b = g & ~r. The active elements are a's: Z when
   * none; N when g's lowest bit is a's (a & -g, as a lies within g); C
   * unless g's highest bit is a's, that is unless a > b, as a and b are
   * disjoint.
   */
  for (const auto [source, gpr] :
       {std::pair{mask, 8U}, std::pair{result, 9U}}) {
    emitter_.emit(0x0f0c'8400U | (source << 5) | scratch_b); // shrn #4
    emitter_.emit(0x9e66'0000U | (scratch_b << 5) | gpr);    // fmov x, d5
  }
  emitter_.emit(and_x(10, 8, 9));
  emitter_.emit(eor_x(11, 8, 10));
  emitter_.emit(sub_x(16, 31, 8));
  emitter_.emit(cmp_x(10, 11));
  emitter_.emit(cset_w(9, cond_ls));
  emitter_.emit(tst_x(10, 16));
  emitter_.emit(cset_w(8, cond_ne));
  emitter_.emit(cmp_immediate(10, 0));
  emitter_.emit(cset_w(16, cond_eq));
  emitter_.emit(lsl_w(9, 9, 29));
  emitter_.emit(orr_w_lsl(9, 9, 8, 31));
  emitter_.emit(orr_w_lsl(11, 9, 16, 30));
  emit_sve_flags_from_w11();
}

void JitCompiler::emit_sve_active_count(unsigned predicate, unsigned esize,
                                        unsigned destination) {
  /* One per active element (the top bit of its first byte), summed. */
  const unsigned bits = esize * 8U;
  unsigned source = predicate;
  if (esize != 1U) {
    emitter_.emit(0x4f00'5400U | ((2U * bits - 8U) << 16) | (source << 5) |
                  scratch_a); // shl #(bits - 8)
    source = scratch_a;
  }
  emitter_.emit(0x6f00'0400U | ((bits + 1U) << 16) | (source << 5) |
                scratch_a); // ushr #(bits - 1)
  if (esize == 8U)
    emitter_.emit(0x5ef1'b800U | (scratch_a << 5) | scratch_a); // addp d
  else
    emitter_.emit(0x4e31'b800U |
                  (static_cast<unsigned>(std::countr_zero(esize)) << 22) |
                  (scratch_a << 5) | scratch_a); // addv
  emitter_.emit(0x1e26'0000U | (scratch_a << 5) | destination); // fmov w, s
}

unsigned JitCompiler::emit_sve_lane_mask(unsigned predicate, unsigned esize,
                                         unsigned scratch) {
  if (esize == 1U)
    return predicate;
  /* Each lane's first byte, 0x00 or 0xff, across the lane. */
  const unsigned bits = esize * 8U;
  emitter_.emit(0x4f00'5400U | ((2U * bits - 8U) << 16) | (predicate << 5) |
                scratch); // shl #(bits - 8)
  emitter_.emit(0x4f00'0400U | ((bits + 1U) << 16) | (scratch << 5) |
                scratch); // sshr #(bits - 1)
  return scratch;
}

void JitCompiler::emit_sve_vector_constant(unsigned destination,
                                           std::uint64_t low,
                                           std::uint64_t high) {
  const auto low_bytes = byte_mask_immediate(low);
  if (low == high) {
    if (low_bytes) {
      emitter_.emit(movi_bytes(true, destination, *low_bytes));
    } else {
      emitter_.load_constant(16, low);
      emitter_.emit(dup_general(3U, destination, 16));
    }
    return;
  }
  if (low_bytes) {
    emitter_.emit(movi_bytes(false, destination, *low_bytes));
  } else {
    emitter_.load_constant(16, low);
    emitter_.emit(fmov_d_x(destination, 16));
  }
  if (high != 0U) {
    emitter_.load_constant(16, high);
    emitter_.emit(ins_high(destination, 16));
  }
}

void JitCompiler::emit_sve_element_constant(unsigned destination,
                                            std::uint64_t element,
                                            unsigned esize) {
  std::uint64_t replicated = element;
  for (unsigned width = esize * 8U; width < 64U; width *= 2U)
    replicated |= replicated << width;
  if (byte_mask_immediate(replicated)) {
    emit_sve_vector_constant(destination, replicated, replicated);
    return;
  }
  emitter_.load_constant(16, element);
  emitter_.emit(dup_general(static_cast<unsigned>(std::countr_zero(esize)),
                            destination, 16));
}

void JitCompiler::emit_sve_predicate_form(std::uint16_t form,
                                          std::uint32_t instruction) {
  const unsigned d = field(instruction, 0, 4);
  if (form == forms::ptrue || form == forms::ptrues ||
      form == forms::pfalse) {
    const unsigned esize = 1U << field(instruction, 22, 2);
    const unsigned count =
        form == forms::pfalse
            ? 0U
            : predicate_pattern_count(field(instruction, 5, 5),
                                      sve_vector_length / esize);
    const auto image = sve_leading_predicate(esize, count);
    const auto out = write_native_predicate(d, false, 3U);
    emit_sve_vector_constant(out.host, predicate_half(image.predicate, 0),
                             predicate_half(image.predicate, 1));
    commit_native_predicate(out, d);
    if (form != forms::pfalse && count == sve_vector_length / esize)
      note_sve_predicate_full(d, esize);
    if (form == forms::ptrues) {
      /* PTRUES tests its result under itself, not under the all-true
       * predicate a WHILE* result is tested under: N when any element is
       * active, Z and C when none is. */
      emitter_.load_constant(11, count != 0U ? 0x8000'0000U : 0x6000'0000U);
      emit_sve_flags_from_w11();
    }
    return;
  }
  if (form == forms::setffr) {
    const auto out = write_native_predicate(sve_ffr, false, 3U);
    emit_sve_vector_constant(out.host, ~std::uint64_t{0}, ~std::uint64_t{0});
    commit_native_predicate(out, sve_ffr);
    return;
  }
  if (form == forms::wrffr) {
    const unsigned n = field(instruction, 5, 4);
    const auto source = read_native_predicate(n, 0U);
    const auto out = write_native_predicate(sve_ffr, false, 3U);
    emitter_.emit(vector_move(out.host, source.host));
    commit_native_predicate(out, sve_ffr);
    return;
  }
  if (form == forms::rdffr) {
    const auto ffr = read_native_predicate(sve_ffr, 0U);
    const auto out = write_native_predicate(d, false, 3U);
    emitter_.emit(vector_move(out.host, ffr.host));
    commit_native_predicate(out, d);
    return;
  }
  if (form == forms::rdffr_predicated || form == forms::rdffrs) {
    const auto ffr = read_native_predicate(sve_ffr, 0U);
    const auto governing = read_native_predicate(field(instruction, 5, 4), 1U);
    if (form == forms::rdffr_predicated) {
      const auto out = write_native_predicate(d, false, 3U);
      emitter_.emit(three(vector_and, out.host, ffr.host, governing.host));
      commit_native_predicate(out, d);
      return;
    }
    emitter_.emit(three(vector_and, scratch_a, ffr.host, governing.host));
    emit_sve_predicate_test(governing.host, scratch_a);
    const auto out = write_native_predicate(d, false, 3U);
    emitter_.emit(vector_move(out.host, scratch_a));
    commit_native_predicate(out, d);
    return;
  }
  const unsigned n = field(instruction, 5, 4);
  const unsigned g = field(instruction, 10, 4);
  const auto governing = read_native_predicate(g, 0U);
  const auto first = n == g ? governing : read_native_predicate(n, 1U);
  if (form == forms::ptest) {
    emit_sve_predicate_test(governing.host, first.host);
    return;
  }
  const unsigned m = field(instruction, 16, 4);
  const auto second = m == g   ? governing
                      : m == n ? first
                               : read_native_predicate(m, 2U);
  const bool sets_flags =
      form == forms::ands_p || form == forms::bics_p ||
      form == forms::eors_p || form == forms::orrs_p ||
      form == forms::orns_p || form == forms::nands_p || form == forms::nors_p;
  /* The flag-setting forms test the result in v4 before it is written,
   * which a destination of Pg would overwrite. */
  const auto out = sets_flags ? NativeVector{scratch_a, true}
                              : write_native_predicate(d, false, 3U);
  if (form == forms::sel_p) {
    emitter_.emit(three(vector_and, scratch_a, first.host, governing.host));
    emitter_.emit(three(vector_bic, scratch_b, second.host, governing.host));
    emitter_.emit(three(vector_orr, out.host, scratch_a, scratch_b));
  } else {
    std::uint32_t inner = vector_and;
    if (form == forms::bic_p || form == forms::bics_p)
      inner = vector_bic;
    else if (form == forms::eor_p || form == forms::eors_p)
      inner = vector_eor;
    else if (form == forms::orr_p || form == forms::orrs_p ||
             form == forms::nor_p || form == forms::nors_p)
      inner = vector_orr;
    else if (form == forms::orn_p || form == forms::orns_p)
      inner = vector_orn;
    emitter_.emit(three(inner, scratch_a, first.host, second.host));
    /* Zeroing: Pg & op, or Pg & ~op for NAND and NOR. */
    if (form == forms::nand_p || form == forms::nands_p ||
        form == forms::nor_p || form == forms::nors_p)
      emitter_.emit(three(vector_bic, out.host, governing.host, scratch_a));
    else
      emitter_.emit(three(vector_and, out.host, scratch_a, governing.host));
  }
  if (!sets_flags) {
    commit_native_predicate(out, d);
    return;
  }
  emit_sve_predicate_test(governing.host, scratch_a);
  const auto written = write_native_predicate(d, false, 3U);
  emitter_.emit(vector_move(written.host, scratch_a));
  commit_native_predicate(written, d);
}

void JitCompiler::emit_sve_count_form(std::uint16_t form,
                                      std::uint32_t instruction) {
  const unsigned size = field(instruction, 22, 2);
  const unsigned esize = 1U << size;
  const unsigned d = field(instruction, 0, 5);
  if (form == forms::cntp) {
    if (d == 31U)
      return; // XZR
    const unsigned g = field(instruction, 10, 4);
    const unsigned n = field(instruction, 5, 4);
    const auto governing = read_native_predicate(g, 0U);
    const auto source = n == g ? governing : read_native_predicate(n, 1U);
    emitter_.emit(three(vector_and, scratch_a, governing.host, source.host));
    const auto out = write_native_gpr(d, false, false, 9U);
    emit_sve_active_count(scratch_a, esize, out.host);
    commit_native_gpr(out, d, false);
    return;
  }
  if (form == forms::incp_scalar || form == forms::decp_scalar ||
      form == forms::incp_vector || form == forms::decp_vector) {
    const bool subtract =
        form == forms::decp_scalar || form == forms::decp_vector;
    const bool vector =
        form == forms::incp_vector || form == forms::decp_vector;
    if (!vector && d == 31U)
      return; // XZR
    const auto source = read_native_predicate(field(instruction, 5, 4), 0U);
    emit_sve_active_count(source.host, esize, 10U);
    if (!vector) {
      const auto out = write_native_gpr(d, false, true, 9U);
      emitter_.emit((subtract ? sub_x : add_x)(out.host, out.host, 10U));
      commit_native_gpr(out, d, false);
      return;
    }
    emitter_.emit(dup_general(size, scratch_b, 10U));
    const auto out = write_native_vector(d, true, 1U);
    emitter_.emit(vector_add(subtract, size, out.host, out.host, scratch_b));
    commit_native_vector(out, d);
    return;
  }
  if (form == forms::rdvl || form == forms::addvl || form == forms::addpl) {
    /* RDVL Xd: VL * imm; ADDVL and ADDPL Xd|SP, Xn|SP: plus VL or PL
     * (VL / 8) times imm. */
    const auto multiple = sign_extend(field(instruction, 5, 6), 6);
    const std::int64_t unit = form == forms::addpl
                                  ? sve_vector_length / 8U
                                  : sve_vector_length;
    const auto value = multiple * unit;
    if (form == forms::rdvl) {
      if (d == 31U)
        return; // XZR
      const auto out = write_native_gpr(d, false, false, 9U);
      emitter_.load_constant(out.host, static_cast<std::uint64_t>(value));
      commit_native_gpr(out, d, false);
      return;
    }
    const unsigned n = field(instruction, 16, 5);
    const auto source = read_native_gpr(n, true, 8U);
    const auto out = write_native_gpr(d, true, false, 9U);
    if (value != 0 || out.host != source.host) {
      if (value < 0)
        emitter_.subtract_immediate(out.host, source.host,
                                    static_cast<unsigned>(-value));
      else
        emitter_.add_immediate(out.host, source.host,
                               static_cast<unsigned>(value));
    }
    commit_native_gpr(out, d, true);
    return;
  }
  /* CNT, INC and DEC: the pattern's element count times imm4 + 1. */
  const unsigned count =
      predicate_pattern_count(field(instruction, 5, 5),
                              sve_vector_length / esize) *
      (field(instruction, 16, 4) + 1U);
  if (form == forms::cnt) {
    if (d == 31U)
      return; // XZR
    const auto out = write_native_gpr(d, false, false, 9U);
    emitter_.load_constant(out.host, count);
    commit_native_gpr(out, d, false);
    return;
  }
  const bool subtract = form == forms::dec_scalar || form == forms::dec_vector;
  if (count == 0U)
    return;
  if (form == forms::inc_scalar || form == forms::dec_scalar) {
    if (d == 31U)
      return; // XZR
    const auto out = write_native_gpr(d, false, true, 9U);
    if (subtract)
      emitter_.subtract_immediate(out.host, out.host, count);
    else
      emitter_.add_immediate(out.host, out.host, count);
    commit_native_gpr(out, d, false);
    return;
  }
  emit_sve_element_constant(scratch_b, count, esize);
  const auto out = write_native_vector(d, true, 1U);
  emitter_.emit(vector_add(subtract, size, out.host, out.host, scratch_b));
  commit_native_vector(out, d);
}

void JitCompiler::emit_sve_move_form(std::uint16_t form,
                                     std::uint32_t instruction) {
  const unsigned size = field(instruction, 22, 2);
  const unsigned esize = 1U << size;
  const unsigned d = field(instruction, 0, 5);
  const unsigned n = field(instruction, 5, 5);
  if (form == forms::and_z || form == forms::orr_z || form == forms::eor_z ||
      form == forms::bic_z) {
    const std::uint32_t operation = form == forms::and_z   ? vector_and
                                    : form == forms::orr_z ? vector_orr
                                    : form == forms::eor_z ? vector_eor
                                                           : vector_bic;
    emit_native_vector_binary(operation | (instruction & 0x001f'03ffU), false);
    return;
  }
  if (form == forms::movprfx) {
    if (d != n)
      emit_native_vector_binary(three(vector_orr, d, n, n), false);
    return;
  }
  if (form == forms::dup_scalar) {
    const auto source = read_native_gpr(n, true, 8U);
    const auto out = write_native_vector(d, false, 0U);
    emitter_.emit(dup_general(size, out.host, source.host));
    commit_native_vector(out, d);
    return;
  }
  if (form == forms::dup_immediate || form == forms::dupm ||
      form == forms::fdup) {
    const auto out = write_native_vector(d, false, 0U);
    if (form == forms::dup_immediate) {
      emit_sve_element_constant(out.host, integer_element(instruction, esize),
                                esize);
    } else if (form == forms::dupm) {
      const auto value = *detail::decode_logical_immediate(
          true, field(instruction, 17, 1), field(instruction, 11, 6),
          field(instruction, 5, 6));
      emit_sve_vector_constant(out.host, value, value);
    } else {
      const unsigned immediate = field(instruction, 5, 8);
      if (size == 1U && !host_capabilities().fp16) {
        emit_sve_element_constant(out.host,
                                  expand_float_immediate(immediate, 16U), 2U);
      } else {
        /* FMOV Vd.8H, Vd.4S or Vd.2D, #imm. */
        const std::uint32_t opcode = size == 1U   ? 0x4f00'fc00U
                                     : size == 2U ? 0x4f00'f400U
                                                  : 0x6f00'f400U;
        emitter_.emit(opcode | ((immediate >> 5) << 16) |
                      ((immediate & 31U) << 5) | out.host);
      }
    }
    commit_native_vector(out, d);
    return;
  }
  if (form == forms::dup_indexed) {
    /* The lowest set bit of tsz gives the element size; the bits above
     * it and imm2 the index. An index past the vector selects zero. */
    const unsigned immediate =
        (field(instruction, 22, 2) << 5) | field(instruction, 16, 5);
    const auto log_size = static_cast<unsigned>(
        std::countr_zero(field(instruction, 16, 5)));
    const unsigned index = immediate >> (log_size + 1U);
    const unsigned elements = sve_vector_length >> log_size;
    if (index >= elements) {
      const auto out = write_native_vector(d, false, 0U);
      emit_sve_vector_constant(out.host, 0U, 0U);
      commit_native_vector(out, d);
      return;
    }
    const auto source = read_native_vector(n, 1U);
    const auto out = write_native_vector(d, false, 0U);
    if (log_size == 4U) {
      if (out.host != source.host)
        emitter_.emit(vector_move(out.host, source.host));
    } else {
      emitter_.emit(dup_element(log_size, index, out.host, source.host));
    }
    commit_native_vector(out, d);
    return;
  }
  /* The predicated forms: Pg in bits 10-12 or 10-13, or 16-19 for an
   * immediate. */
  const bool immediate_form = form == forms::cpy_merging ||
                              form == forms::cpy_zeroing ||
                              form == forms::fcpy;
  const unsigned g =
      immediate_form ? field(instruction, 16, 4)
      : form == forms::sel_z ? field(instruction, 10, 4)
                             : field(instruction, 10, 3);
  const auto governing = read_native_predicate(g, 0U);
  const unsigned mask = emit_sve_lane_mask(governing.host, esize, scratch_a);
  if (form == forms::sel_z) {
    const unsigned m = field(instruction, 16, 5);
    const auto first = read_native_vector(n, 1U);
    const auto second = m == n ? first : read_native_vector(m, 2U);
    const auto out = write_native_vector(d, false, 3U);
    if (out.host == second.host) {
      emitter_.emit(three(vector_bit, out.host, first.host, mask));
    } else if (out.host == first.host) {
      emitter_.emit(three(vector_bif, out.host, second.host, mask));
    } else {
      emitter_.emit(vector_move(out.host, second.host));
      emitter_.emit(three(vector_bit, out.host, first.host, mask));
    }
    commit_native_vector(out, d);
    return;
  }
  if (form == forms::movprfx_predicated) {
    const bool merging = field(instruction, 16, 1) != 0U;
    if (merging && d == n)
      return;
    const auto source = read_native_vector(n, 1U);
    const auto out = write_native_vector(d, merging, 2U);
    emitter_.emit(three(merging ? vector_bit : vector_and, out.host,
                        source.host, mask));
    commit_native_vector(out, d);
    return;
  }
  /* CPY and FCPY: the value in every lane of v5, then merged or zeroed. */
  if (form == forms::cpy_scalar) {
    const auto source = read_native_gpr(n, true, 8U);
    emitter_.emit(dup_general(size, scratch_b, source.host));
  } else if (form == forms::cpy_simd) {
    const auto source = read_native_vector(n, 1U);
    emitter_.emit(dup_element(size, 0U, scratch_b, source.host));
  } else if (form == forms::fcpy) {
    emit_sve_element_constant(
        scratch_b, expand_float_immediate(field(instruction, 5, 8), esize * 8U),
        esize);
  } else {
    emit_sve_element_constant(scratch_b, integer_element(instruction, esize),
                              esize);
  }
  const bool zeroing = form == forms::cpy_zeroing;
  const auto out = write_native_vector(d, !zeroing, 2U);
  emitter_.emit(three(zeroing ? vector_and : vector_bit, out.host, scratch_b,
                      mask));
  commit_native_vector(out, d);
}

bool JitCompiler::emit_sve_while(std::uint16_t form,
                                 std::uint32_t instruction, std::uint64_t pc) {
  const unsigned log_size = field(instruction, 22, 2);
  const unsigned d = field(instruction, 0, 4);
  const unsigned rn = field(instruction, 5, 5);
  const unsigned rm = field(instruction, 16, 5);
  /* XZR reaches only register-form operands below; never SP. */
  const auto operand = [&](unsigned guest, unsigned fallback) {
    auto value = read_native_gpr(guest, false, fallback);
    if (value.host == 31U) {
      emitter_.load_constant(fallback, 0);
      value.host = fallback;
    }
    return value.host;
  };
  bool trailing = false;
  constexpr unsigned all = 9U; // x9 = 16, every element active
  if (form == forms::whilewr || form == forms::whilerw) {
    /* x10 = |op2 - op1| for RW; op2 - op1, or 0 when negative, for WR;
     * in elements, at most 16, and 0 means every element. */
    const unsigned first = operand(rn, 0U);
    const unsigned second = operand(rm, 1U);
    emitter_.emit(subs_x(10, second, first));
    if (form == forms::whilerw) {
      emitter_.emit(sub_x(11, first, second));
      emitter_.emit(csel_x(10, 10, 11, cond_hs));
    } else {
      emitter_.emit(csel_x(10, 10, 31, cond_hs));
    }
    if (log_size != 0U)
      emitter_.emit(lsr_x(10, 10, log_size));
    emitter_.load_constant(all, 16);
    emitter_.emit(cmp_immediate(10, 16));
    emitter_.emit(csel_x(10, 10, all, cond_lo));
    emitter_.emit(cmp_immediate(10, 0));
    emitter_.emit(csel_x(10, all, 10, cond_eq));
  } else {
    /*
     * Operand 1 steps one element at a time towards operand 2 (bit 10: up),
     * and the elements active are those before the comparison first fails:
     * the distance, plus one when inclusive, at most 16. The comparison
     * fails before operand 1 wraps unless an inclusive comparison's bound
     * is the extreme value, when every element is active.
     */
    const bool wide = field(instruction, 12, 1) != 0U;
    const bool is_unsigned = field(instruction, 11, 1) != 0U;
    const bool increasing = field(instruction, 10, 1) != 0U;
    const bool equal = field(instruction, 4, 1) != 0U;
    const bool inclusive = increasing == equal;
    trailing = !increasing;
    unsigned first = operand(rn, 0U);
    unsigned second = operand(rm, 1U);
    if (!wide) {
      emitter_.emit(is_unsigned ? uxtw(0, first) : sxtw(0, first));
      emitter_.emit(is_unsigned ? uxtw(1, second) : sxtw(1, second));
      first = 0U;
      second = 1U;
    }
    if (increasing)
      emitter_.emit(sub_x(10, second, first));
    else
      emitter_.emit(sub_x(10, first, second));
    emitter_.load_constant(all, 16);
    if (inclusive) {
      emitter_.emit(cmp_immediate(10, 15));
      emitter_.emit(csinc_x(10, all, 10, cond_hs));
    } else {
      emitter_.emit(cmp_immediate(10, 16));
      emitter_.emit(csel_x(10, 10, all, cond_lo));
    }
    unsigned holds;
    if (increasing)
      holds = inclusive ? (is_unsigned ? cond_ls : cond_le)
                        : (is_unsigned ? cond_lo : cond_lt);
    else
      holds = inclusive ? (is_unsigned ? cond_hs : cond_ge)
                        : (is_unsigned ? cond_hi : cond_gt);
    emitter_.emit(cmp_x(first, second));
    emitter_.emit(csel_x(10, 10, 31, holds));
    if (inclusive) {
      if (!increasing && is_unsigned) {
        emitter_.emit(cmp_immediate(second, 0)); // HS: bound 0
      } else if (increasing && is_unsigned) {
        emitter_.emit(cmn_immediate(second, wide, 1)); // LS: bound all ones
      } else {
        const std::uint64_t bound =
            increasing ? (wide ? 0x7fff'ffff'ffff'ffffULL : 0x7fff'ffffULL)
                       : (wide ? 0x8000'0000'0000'0000ULL
                               : 0xffff'ffff'8000'0000ULL);
        emitter_.load_constant(11, bound);
        emitter_.emit(cmp_x(second, 11));
      }
      emitter_.emit(csel_x(10, all, 10, cond_eq));
    }
  }
  /* The predicate and its NZCV from the count's table entry. */
  const auto &table = trailing ? while_tables<true>[log_size]
                               : while_tables<false>[log_size];
  emitter_.load_stable_pointer(16, reinterpret_cast<std::uintptr_t>(&table));
  emitter_.add_shifted(16, 16, 10, while_entry_shift);
  const auto out = write_native_predicate(d, false, 3U);
  emitter_.load_q(out.host, 16, offsetof(WhileEntry, predicate));
  commit_native_predicate(out, d);
  emitter_.load_w(11, 16, offsetof(WhileEntry, nzcv));
  /*
   * Fusion (design 4.1.7): a B.cond after the WHILE in this block that
   * tests one flag (b.none, b.any, b.nlast, b.last, b.first, b.nfrst)
   * branches on that bit of the entry's NZCV, which is stored for the
   * successors, and the pair ends the block.
   */
  const auto branch_index = count_ + 1U;
  if (branch_index < block_.size()) {
    const auto branch = block_[branch_index];
    const unsigned condition = branch & 15U;
    if ((branch & 0xff00'0010U) == 0x5400'0000U && condition <= cond_pl) {
      constexpr std::array<unsigned, 3> bits{30U, 29U, 31U}; // Z, C, N
      const unsigned bit = bits[condition >> 1];
      const bool taken_when_set = (condition & 1U) == 0U;
      emitter_.store_w(11, 15, offsetof(CpuState, nzcv));
      assume_flags_in_memory();
      const auto branch_pc = pc + 4U;
      const auto target =
          branch_pc +
          static_cast<std::uint64_t>(
              sign_extend((branch >> 5) & 0x7'ffffU, 19) * 4);
      const auto retired = count_ + 2U;
      const auto not_taken = make_label();
      test_zero(not_taken, 11, bit, !taken_when_set);
      emit_direct_branch(target, retired);
      bind(not_taken);
      emit_direct_branch(branch_pc + 4U, retired);
      ++count_;
      return true;
    }
  }
  emit_sve_flags_from_w11();
  return false;
}

} // namespace uml::cpu::arm64
