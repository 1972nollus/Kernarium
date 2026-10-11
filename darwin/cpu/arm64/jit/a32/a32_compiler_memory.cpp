// SPDX-License-Identifier: GPL-2.0-or-later
//
// Memory: the inline lowering of the transfer instructions through the
// shared TLB fast path, and the cold edges to the slow helper.
//
// An address is a W value zero-extended into x8, which the translation
// replaces in place by the host pointer. A page crossing, a misalignment the
// architecture faults on and every exclusive take the cold path to
// uml_arm64_jit_a32_execute, which runs the whole instruction; the hot path
// therefore changes no architectural state before it knows the access
// completes. The translation clobbers x0, x1, x2, x9 and, on a miss, every
// other scratch register, so a register the access needs afterwards is read
// again from its resident host or CpuState (a writeback recomputes its
// value for that reason).

#include "a32_compiler.hpp"

#include "a32_encoders.hpp"
#include "a32_helpers.hpp"
#include "a32_operations.hpp"

#include "cpu/arm64/memory.hpp"
#include "memory/tlb.hpp"

#include <bit>

namespace uml::cpu::arm64::jit::a32 {

using ::uml::memory::Access;
using aarch32::SRType;

namespace {

constexpr unsigned fallback_n = 9;
constexpr unsigned fallback_m = 10;
constexpr unsigned writeback_m = 1;
constexpr unsigned address_register = 8;
constexpr unsigned temp0 = 0;

} // namespace

// ---------------------------------------------------------------------------
// The shared TLB fast path (a copy of JitCompiler::emit_translate; the A64
// compiler's is a member of a class this one is not part of).

void A32Compiler::emit_translate(unsigned address, Access access,
                                 Arm64Emitter::Label fallback,
                                 Arm64Emitter::Label fault,
                                 std::size_t bytes) {
  if (bytes > 1) {
    emitter_.ubfm(0, address, 0, page_shift - 1);
    emitter_.add_immediate(0, 0, static_cast<unsigned>(bytes - 1));
    /* bytes is below one page, so the sum is below two pages and its page
     * bit is the complete crossing test. */
    emitter_.test_zero(fallback, 0, page_shift, true);
  }
  emitter_.ubfm(1, address, page_shift, 63U); // lsr x1, Xn, #page_shift
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
  emitter_.ubfm(0, address, page_shift,
                page_shift + ::uml::memory::tlb_index_bits - 1U);
  emitter_.add_shifted(0, 14, 0, 6);
#else
  emitter_.ubfm(0, address, page_shift,
                page_shift + ::uml::memory::tlb_index_bits - 1U);
  emitter_.emit(0xca40'0000U | ((page_shift + 13U) << 10) |
                ((address & 31U) << 16)); // eor x0, x0, address, lsr #25
  emitter_.ubfm(0, 0, 0, ::uml::memory::tlb_index_bits - 1U);
  emitter_.add_shifted(0, 14, 0, 5);
#endif
  const unsigned page_offset = access == Access::write ? 16U : 0U;
  const auto slow = emitter_.make_label();
  const auto complete = emitter_.make_label();
  emitter_.load_pair_x(2, 9, 0, page_offset);
  emitter_.emit(0xca01'0042U); // eor x2, x2, x1
  emitter_.compare_zero(slow, 2, true, true);
  emitter_.add_shifted(address, address, 9, 0);
  emitter_.bind(complete);
  translation_cold_.push_back({
      .label = slow,
      .continuation = complete,
      .fault = fault,
      .address = address,
      .access = access,
  });
}

void A32Compiler::emit_translation_cold(const TranslationColdPath &path) {
  emitter_.bind(path.label);
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
  const unsigned page_offset = path.access == Access::write ? 16U : 0U;
  const auto miss = emitter_.make_label();
  emitter_.load_pair_x(2, 9, 0, page_offset + 32U);
  emitter_.emit(0xca01'0042U); // eor x2, x2, x1
  emitter_.compare_zero(miss, 2, true, true);
  emitter_.add_shifted(path.address, path.address, 9, 0);
  emitter_.branch(path.continuation);
  emitter_.bind(miss);
#endif
  emitter_.move_x(0, 14);
  emitter_.move_x(1, path.address);
  emitter_.load_constant(2, path.access == Access::write ? 1U : 0U);
  emitter_.call_preserving_flags(
      reinterpret_cast<const void *>(&uml_arm64_jit_tlb_translate_preserving));
  emitter_.move_x(path.address, 0);
  emitter_.compare_zero(path.fault, path.address, true, false);
  emitter_.branch(path.continuation);
}

void A32Compiler::emit_alignment_check(unsigned address, unsigned alignment,
                                       Arm64Emitter::Label slow) {
  if (alignment <= 1U)
    return;
  emitter_.ubfm(0, address, 0,
                static_cast<unsigned>(std::countr_zero(alignment) - 1U));
  emitter_.compare_zero(slow, 0, true, true);
}

// ---------------------------------------------------------------------------
// Store bookkeeping (copies of the A64 compiler's).

void A32Compiler::mark_memory_write() noexcept {
  if (count_ < maximum_block_instructions)
    memory_write_mask_ |= std::uint64_t{1} << count_;
}

void A32Compiler::mark_direct_memory_write() noexcept {
  mark_memory_write();
  if (count_ < maximum_block_instructions)
    direct_memory_write_mask_ |= std::uint64_t{1} << count_;
}

/* Records the range a tracked block stores to, for the software reservation
 * table. `address` is the permission-checked host pointer. */
void A32Compiler::track_direct_store(unsigned address, std::size_t size) {
  if (!track_direct_stores_ || count_ >= maximum_block_instructions)
    return;
  const auto offset = count_ * sizeof(JitStoreRange);
  emitter_.load_x(3, 31, 32);
  emitter_.load_x(3, 3, offsetof(JitChainState, stores));
  emitter_.store_x(address, 3, offset + offsetof(JitStoreRange, address));
  emitter_.load_constant(4, size);
  emitter_.store_x(4, 3, offset + offsetof(JitStoreRange, size));
}

/* An ordinary store may clear the local monitor, and does, unconditionally. */
void A32Compiler::clear_exclusive_monitor() {
  emitter_.store_b(31, 15, offsetof(CpuState, exclusive_size));
}

// ---------------------------------------------------------------------------
// The dispatcher.

A32Compiler::Flow A32Compiler::emit_memory(const OpInfo &) {
  const auto transfer = describe_transfer(cur().inst);
  switch (transfer.kind) {
  case Transfer::Kind::single: return emit_single(transfer);
  case Transfer::Kind::pair: return emit_pair(transfer);
  case Transfer::Kind::multiple: return emit_multiple(transfer);
  case Transfer::Kind::table: return emit_table(transfer);
  case Transfer::Kind::exclusive: return emit_exclusive(transfer);
  case Transfer::Kind::prefetch: return Flow::next;
  case Transfer::Kind::none: break;
  }
  exit_illegal();
  return Flow::end;
}

/* The instruction in the slow helper, which retires it and ends the block. */
A32Compiler::Flow A32Compiler::emit_through_helper() {
  const auto transfer = describe_transfer(cur().inst);
  if (!transfer.load)
    mark_memory_write();
  if (transfer.kind == Transfer::Kind::exclusive) {
    mark_memory_write();
    emitter_.branch(cold_exit(ColdPath::Kind::exclusive_slow));
  } else {
    emitter_.branch(cold_exit(ColdPath::Kind::slow));
  }
  return Flow::end;
}

// ---------------------------------------------------------------------------
// Addresses.

void A32Compiler::form_address(const Transfer &transfer) {
  if (transfer.literal) {
    const std::uint32_t base = pc_align_read();
    load_u32(address_register,
             transfer.add ? base + transfer.imm : base - transfer.imm);
    return;
  }
  const unsigned n = use(transfer.n, fallback_n);
  if (!transfer.index || (!transfer.register_offset && transfer.imm == 0U)) {
    if (n != address_register)
      emit(enc::mov(address_register, n));
    return;
  }
  if (!transfer.register_offset) {
    emit(transfer.add ? enc::add_i(address_register, n, transfer.imm)
                      : enc::sub_i(address_register, n, transfer.imm));
    return;
  }
  const unsigned m = use(transfer.m, fallback_m);
  enc::Shift fold = enc::lsl;
  unsigned amount = 0;
  unsigned operand = m;
  switch (transfer.shift_type) {
  case SRType::LSL:
    amount = transfer.shift_amount;
    break;
  case SRType::LSR:
    if (transfer.shift_amount < 32U) {
      fold = enc::lsr;
      amount = transfer.shift_amount;
    } else {
      operand = shift_value(m, SRType::LSR, transfer.shift_amount, temp0);
    }
    break;
  case SRType::ASR:
    fold = enc::asr;
    amount = transfer.shift_amount >= 32U ? 31U : transfer.shift_amount;
    break;
  default:
    operand = shift_value(m, transfer.shift_type, transfer.shift_amount, temp0);
    break;
  }
  emit(transfer.add ? enc::add(address_register, n, operand, fold, amount)
                    : enc::sub(address_register, n, operand, fold, amount));
}

/* The base register's new value: base plus or minus the offset, computed
 * from the registers as they stand now (a load's destination is never the
 * offset register: that case takes the helper). */
void A32Compiler::write_back(const Transfer &transfer) {
  const auto dest = def(transfer.n, true, fallback_m);
  const unsigned host = dest.native.host;
  if (!transfer.register_offset) {
    if (transfer.imm != 0U)
      emit(transfer.add ? enc::add_i(host, host, transfer.imm)
                        : enc::sub_i(host, host, transfer.imm));
  } else {
    const unsigned m = use(transfer.m, writeback_m);
    enc::Shift fold = enc::lsl;
    unsigned amount = 0;
    unsigned operand = m;
    switch (transfer.shift_type) {
    case SRType::LSL:
      amount = transfer.shift_amount;
      break;
    case SRType::LSR:
      if (transfer.shift_amount < 32U) {
        fold = enc::lsr;
        amount = transfer.shift_amount;
      } else {
        operand = shift_value(m, SRType::LSR, transfer.shift_amount, temp0);
      }
      break;
    case SRType::ASR:
      fold = enc::asr;
      amount = transfer.shift_amount >= 32U ? 31U : transfer.shift_amount;
      break;
    default:
      operand =
          shift_value(m, transfer.shift_type, transfer.shift_amount, temp0);
      break;
    }
    emit(transfer.add ? enc::add(host, host, operand, fold, amount)
                      : enc::sub(host, host, operand, fold, amount));
  }
  done(dest);
}

// ---------------------------------------------------------------------------
// Single transfers.

A32Compiler::Flow A32Compiler::emit_single(const Transfer &transfer) {
  /* A load whose destination is also the offset register of a writeback form
   * would feed the loaded value to the recomputation of the base, so it loads
   * into a scratch register, recomputes the base from the registers as they
   * stand, and then writes the destination. The base as the destination or
   * the PC anywhere is unpredictable: the helper runs those. The PC as a
   * store's offset register is excluded by the decoder. */
  const bool over_offset = transfer.wback && transfer.register_offset &&
                           transfer.load && transfer.t == transfer.m;
  if (over_offset && (transfer.n == transfer.t || transfer.n == 15U ||
                      transfer.t == 15U))
    return emit_through_helper();
  const bool store = !transfer.load;
  if (store)
    mark_direct_memory_write();
  const auto slow = cold_exit(ColdPath::Kind::slow);
  const auto fault = cold_exit(ColdPath::Kind::memory_fault);
  form_address(transfer);
  /* A load-acquire and a store-release fault unless aligned to the size; the
   * helper reports it. */
  if (transfer.ordered)
    emit_alignment_check(address_register, transfer.bytes, slow);
  emit_translate(address_register, store ? Access::write : Access::read, slow,
                 fault, transfer.bytes);
  const unsigned size_field = transfer.bytes == 1U ? 0U
                              : transfer.bytes == 2U ? 1U
                                                     : 2U;
  if (store) {
    track_direct_store(address_register, transfer.bytes);
    const unsigned value =
        transfer.t == 15U ? (load_u32(fallback_n, pc_read()), fallback_n)
                          : use(transfer.t, fallback_n);
    if (transfer.ordered) {
      emit(enc::stlr(size_field, value, address_register));
    } else {
      switch (transfer.bytes) {
      case 1: emit(enc::strb(value, address_register, 0)); break;
      case 2: emit(enc::strh(value, address_register, 0)); break;
      default: emit(enc::str_w(value, address_register, 0)); break;
      }
    }
    clear_exclusive_monitor();
    if (transfer.wback)
      write_back(transfer);
    return Flow::next;
  }
  const bool writes_pc = transfer.t == 15U;
  Dest dest{};
  unsigned target = fallback_n;
  if (!writes_pc && !over_offset) {
    dest = def(transfer.t, false, fallback_n);
    target = dest.native.host;
  }
  if (transfer.ordered) {
    emit(enc::ldar(size_field, target, address_register));
  } else {
    switch (transfer.bytes) {
    case 1:
      emit(transfer.sign_extend ? enc::ldrsb(target, address_register, 0)
                                : enc::ldrb(target, address_register, 0));
      break;
    case 2:
      emit(transfer.sign_extend ? enc::ldrsh(target, address_register, 0)
                                : enc::ldrh(target, address_register, 0));
      break;
    default:
      emit(enc::ldr_w(target, address_register, 0));
      break;
    }
  }
  if (over_offset) {
    write_back(transfer);
    dest = def(transfer.t, false, fallback_n);
    if (dest.native.host != fallback_n)
      emit(enc::mov(dest.native.host, fallback_n));
    done(dest);
    return Flow::next;
  }
  if (!writes_pc)
    done(dest);
  if (transfer.wback)
    write_back(transfer);
  if (writes_pc) {
    write_pc(target, true);
    return Flow::end;
  }
  return Flow::next;
}

// ---------------------------------------------------------------------------
// LDRD and STRD.

A32Compiler::Flow A32Compiler::emit_pair(const Transfer &transfer) {
  if (transfer.wback && transfer.register_offset && transfer.load &&
      (transfer.t == transfer.m || transfer.t2 == transfer.m))
    return emit_through_helper();
  const bool store = !transfer.load;
  if (store)
    mark_direct_memory_write();
  const auto slow = cold_exit(ColdPath::Kind::slow);
  const auto fault = cold_exit(ColdPath::Kind::memory_fault);
  form_address(transfer);
  /* MemA: a doubleword transfer faults unless word aligned; the helper
   * reports it. */
  emit_alignment_check(address_register, 4, slow);
  emit_translate(address_register, store ? Access::write : Access::read, slow,
                 fault, 8);
  if (store) {
    track_direct_store(address_register, 8);
    const unsigned first = use(transfer.t, fallback_n);
    const unsigned second = use(transfer.t2, fallback_m);
    emit(enc::x::orr(temp0, first, second, enc::lsl, 32));
    emit(enc::str_x(temp0, address_register, 0));
    clear_exclusive_monitor();
  } else {
    emit(enc::ldr_x(fallback_m, address_register, 0));
    const auto low = def(transfer.t, false, fallback_n);
    emit(enc::mov(low.native.host, fallback_m));
    done(low);
    const auto high = def(transfer.t2, false, fallback_n);
    emit(enc::x::lsr_i(high.native.host, fallback_m, 32));
    done(high);
  }
  if (transfer.wback)
    write_back(transfer);
  return Flow::next;
}

// ---------------------------------------------------------------------------
// LDM, STM, PUSH and POP.

A32Compiler::Flow A32Compiler::emit_multiple(const Transfer &transfer) {
  const unsigned count = std::popcount(static_cast<unsigned>(transfer.registers));
  const unsigned span = 4U * count;
  const bool store = !transfer.load;
  if (store)
    mark_direct_memory_write();
  const auto slow = cold_exit(ColdPath::Kind::slow);
  const auto fault = cold_exit(ColdPath::Kind::memory_fault);
  const unsigned n = use(transfer.n, fallback_n);
  switch (transfer.mode) {
  case Transfer::Mode::ia:
    if (n != address_register)
      emit(enc::mov(address_register, n));
    break;
  case Transfer::Mode::ib:
    emit(enc::add_i(address_register, n, 4));
    break;
  case Transfer::Mode::da:
    emit(enc::sub_i(address_register, n, span - 4U));
    break;
  case Transfer::Mode::db:
    emit(enc::sub_i(address_register, n, span));
    break;
  }
  emit_alignment_check(address_register, 4, slow);
  emit_translate(address_register, store ? Access::write : Access::read, slow,
                 fault, span);
  if (store)
    track_direct_store(address_register, span);
  unsigned offset = 0;
  bool writes_pc = false;
  for (unsigned r = 0; r < 16U; ++r) {
    if ((transfer.registers >> r & 1U) == 0U)
      continue;
    if (store) {
      const unsigned value =
          r == 15U ? (load_u32(fallback_n, pc_read()), fallback_n)
                   : use(r, fallback_n);
      emit(enc::str_w(value, address_register, offset));
    } else if (r == 15U) {
      emit(enc::ldr_w(fallback_n, address_register, offset));
      writes_pc = true;
    } else {
      const auto dest = def(r, false, fallback_n);
      emit(enc::ldr_w(dest.native.host, address_register, offset));
      done(dest);
    }
    offset += 4U;
  }
  if (store)
    clear_exclusive_monitor();
  if (transfer.wback) {
    const auto dest = def(transfer.n, true, fallback_m);
    const unsigned host = dest.native.host;
    const bool up = transfer.mode == Transfer::Mode::ia ||
                    transfer.mode == Transfer::Mode::ib;
    emit(up ? enc::add_i(host, host, span) : enc::sub_i(host, host, span));
    done(dest);
  }
  if (writes_pc) {
    write_pc(fallback_n, true);
    return Flow::end;
  }
  return Flow::next;
}

// ---------------------------------------------------------------------------
// TBB and TBH.

A32Compiler::Flow A32Compiler::emit_table(const Transfer &transfer) {
  const auto slow = cold_exit(ColdPath::Kind::slow);
  const auto fault = cold_exit(ColdPath::Kind::memory_fault);
  const unsigned n = use(transfer.n, fallback_n);
  const unsigned m = use(transfer.m, fallback_m);
  emit(enc::add(address_register, n, m, enc::lsl, transfer.halfword_table ? 1U : 0U));
  emit_translate(address_register, Access::read, slow, fault, transfer.bytes);
  emit(transfer.halfword_table ? enc::ldrh(fallback_n, address_register, 0)
                               : enc::ldrb(fallback_n, address_register, 0));
  emit(enc::lsl_i(fallback_n, fallback_n, 1));
  load_u32(fallback_m, pc_read());
  emit(enc::add(fallback_n, fallback_n, fallback_m));
  build_thumb_key(fallback_n);
  exit_indirect(address_register);
  return Flow::end;
}

// ---------------------------------------------------------------------------
// Exclusives.

A32Compiler::Flow A32Compiler::emit_exclusive(const Transfer &) {
  return emit_through_helper();
}

} // namespace uml::cpu::arm64::jit::a32
