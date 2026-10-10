// SPDX-License-Identifier: GPL-2.0-or-later
//
// The arithmetic forms the translation runs in C++ (a32_arith.hpp): parallel
// add and subtract with their GE flags, saturation of halfword pairs and the
// sum of absolute differences. They never fault and never write the PC.
//
// Interim: the AArch32 semantic library (design 3.2, lane A32-S) replaces
// this file when it exists.

#include "a32_arith.hpp"

#include "a32_operations.hpp"

#include <algorithm>
#include <cstdint>

namespace uml::cpu::arm64::jit::a32 {

namespace {

[[nodiscard]] std::int64_t saturate_signed(std::int64_t value, unsigned bits,
                                           bool &saturated) noexcept {
  const std::int64_t high = (std::int64_t{1} << (bits - 1U)) - 1;
  const std::int64_t low = -(std::int64_t{1} << (bits - 1U));
  if (value > high) {
    saturated = true;
    return high;
  }
  if (value < low) {
    saturated = true;
    return low;
  }
  return value;
}

[[nodiscard]] std::int64_t saturate_unsigned(std::int64_t value, unsigned bits,
                                             bool &saturated) noexcept {
  const std::int64_t high = bits == 0U ? 0 : (std::int64_t{1} << bits) - 1;
  if (value > high) {
    saturated = true;
    return high;
  }
  if (value < 0) {
    saturated = true;
    return 0;
  }
  return value;
}

/* A signed or unsigned lane of `bits` bits at `index` of a word. */
[[nodiscard]] std::int64_t lane(std::uint32_t word, unsigned bits,
                                unsigned index, bool is_signed) noexcept {
  const unsigned shift = index * bits;
  const std::uint32_t mask = bits == 32U ? ~0U : (1U << bits) - 1U;
  const std::uint32_t raw = word >> shift & mask;
  if (!is_signed)
    return raw;
  const std::uint32_t sign = 1U << (bits - 1U);
  return static_cast<std::int64_t>(raw ^ sign) - static_cast<std::int64_t>(sign);
}

void parallel(CpuState &cpu, const aarch32::Instruction &inst,
              const OpInfo &info) noexcept {
  const auto &o = inst.operands;
  const std::uint32_t n = static_cast<std::uint32_t>(cpu.registers[o.n]);
  const std::uint32_t m = static_cast<std::uint32_t>(cpu.registers[o.m]);
  const bool is_signed = info.mode == ParallelMode::signed_wrap ||
                         info.mode == ParallelMode::saturating ||
                         info.mode == ParallelMode::signed_halving;
  const bool wraps = info.mode == ParallelMode::signed_wrap ||
                     info.mode == ParallelMode::unsigned_wrap;
  const bool saturates = info.mode == ParallelMode::saturating ||
                         info.mode == ParallelMode::unsigned_saturating;
  const bool halves = info.mode == ParallelMode::signed_halving ||
                      info.mode == ParallelMode::unsigned_halving;
  const bool sixteen = info.kind == ParallelKind::add16 ||
                       info.kind == ParallelKind::sub16 ||
                       info.kind == ParallelKind::asx ||
                       info.kind == ParallelKind::sax;
  const unsigned bits = sixteen ? 16U : 8U;
  const unsigned lanes = 32U / bits;
  std::uint32_t result = 0;
  std::uint32_t ge = 0;
  for (unsigned i = 0; i < lanes; ++i) {
    // Which operand lane pairs with this one, and whether it adds.
    unsigned m_index = i;
    bool add = true;
    switch (info.kind) {
    case ParallelKind::add16: case ParallelKind::add8:
      break;
    case ParallelKind::sub16: case ParallelKind::sub8:
      add = false;
      break;
    case ParallelKind::asx:
      // Rd[15:0] = Rn[15:0] - Rm[31:16]; Rd[31:16] = Rn[31:16] + Rm[15:0].
      m_index = 1U - i;
      add = i == 1U;
      break;
    case ParallelKind::sax:
      // Rd[15:0] = Rn[15:0] + Rm[31:16]; Rd[31:16] = Rn[31:16] - Rm[15:0].
      m_index = 1U - i;
      add = i == 0U;
      break;
    }
    const std::int64_t a = lane(n, bits, i, is_signed);
    const std::int64_t b = lane(m, bits, m_index, is_signed);
    std::int64_t value = add ? a + b : a - b;
    bool lane_ge = false;
    if (wraps) {
      if (is_signed)
        lane_ge = value >= 0;
      else
        lane_ge = add ? value >= (std::int64_t{1} << bits) : value >= 0;
    } else if (saturates) {
      bool saturated = false;
      value = is_signed ? saturate_signed(value, bits, saturated)
                        : saturate_unsigned(value, bits, saturated);
    } else if (halves) {
      value >>= 1;
    }
    const std::uint32_t mask = (1U << bits) - 1U;
    result |= (static_cast<std::uint32_t>(value) & mask) << (i * bits);
    // GE is one flag per lane, widened to the bytes the lane covers.
    if (lane_ge)
      ge |= mask << (i * bits);
  }
  if (wraps)
    cpu.a32_ge_mask = ge;
  cpu.registers[o.d] = result;
}

} // namespace

bool is_helper_arithmetic(const aarch32::Instruction &inst) noexcept {
  switch (op_info(inst.form).op) {
  case Op::parallel: case Op::ssat16: case Op::usat16: case Op::usad8:
  case Op::usada8:
    return true;
  default:
    return false;
  }
}

void run_helper_arithmetic(CpuState &cpu,
                           const aarch32::Instruction &inst) noexcept {
  const auto info = op_info(inst.form);
  const auto &o = inst.operands;
  switch (info.op) {
  case Op::parallel:
    parallel(cpu, inst, info);
    return;
  case Op::ssat16: case Op::usat16: {
    const std::uint32_t n = static_cast<std::uint32_t>(cpu.registers[o.n]);
    const unsigned sat = static_cast<unsigned>(o.saturate_to);
    bool saturated = false;
    std::uint32_t result = 0;
    for (unsigned i = 0; i < 2; ++i) {
      const std::int64_t value = lane(n, 16, i, true);
      const std::int64_t out = info.op == Op::ssat16
                                   ? saturate_signed(value, sat, saturated)
                                   : saturate_unsigned(value, sat, saturated);
      result |= (static_cast<std::uint32_t>(out) & 0xffffU) << (16U * i);
    }
    if (saturated)
      cpu.a32_q = 1;
    cpu.registers[o.d] = result;
    return;
  }
  case Op::usad8: case Op::usada8: {
    const std::uint32_t n = static_cast<std::uint32_t>(cpu.registers[o.n]);
    const std::uint32_t m = static_cast<std::uint32_t>(cpu.registers[o.m]);
    std::uint32_t sum = 0;
    for (unsigned i = 0; i < 4; ++i) {
      const std::int32_t a = static_cast<std::int32_t>(n >> (8U * i) & 0xffU);
      const std::int32_t b = static_cast<std::int32_t>(m >> (8U * i) & 0xffU);
      sum += static_cast<std::uint32_t>(a > b ? a - b : b - a);
    }
    if (info.op == Op::usada8)
      sum += static_cast<std::uint32_t>(cpu.registers[o.a]);
    cpu.registers[o.d] = sum;
    return;
  }
  default:
    return;
  }
}

} // namespace uml::cpu::arm64::jit::a32
