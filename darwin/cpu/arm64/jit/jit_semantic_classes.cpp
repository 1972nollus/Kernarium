// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_semantic_instructions.hpp"

#include "cpu/arm64/detail/atomic_floating.hpp"
#include "cpu/arm64/detail/read_check_write.hpp"

#include <cstdint>

/*
 * Class selection for the semantic slow path. Each predicate names an
 * allocation that the compiler routes to the corresponding helper; the
 * compiler has already resolved register, feature, and reserved-encoding
 * legality before emitting the call. Where two allocations overlap, the more
 * specific one is tested first.
 */

namespace uml::cpu::arm64::jit::semantic {
namespace {

[[nodiscard]] constexpr bool matches(std::uint32_t instruction,
                                     std::uint32_t mask,
                                     std::uint32_t value) noexcept {
  return (instruction & mask) == value;
}

[[nodiscard]] constexpr bool load_store_64b_form(
    std::uint32_t instruction) noexcept {
  return matches(instruction, 0xffff'fc00U, 0xf83f'd000U) ||  // LD64B
         matches(instruction, 0xffff'fc00U, 0xf83f'9000U) ||  // ST64B
         matches(instruction, 0xffe0'fc00U, 0xf820'b000U) ||  // ST64BV
         matches(instruction, 0xffe0'fc00U, 0xf820'a000U);    // ST64BV0
}

[[nodiscard]] constexpr bool read_check_write_pair(
    std::uint32_t instruction) noexcept {
  const auto decoded = detail::decode_read_check_write(instruction);
  return decoded.recognized && decoded.pair;
}

[[nodiscard]] constexpr bool read_check_write_scalar(
    std::uint32_t instruction) noexcept {
  const auto decoded = detail::decode_read_check_write(instruction);
  return decoded.recognized && !decoded.pair;
}

} // namespace

Handler memory_handler(std::uint32_t instruction) noexcept {
  /* LDAP1 and STL1; LDAPUR and STLUR to SIMD&FP registers. */
  if (matches(instruction, 0xbfbf'fc00U, 0x0d01'8400U) ||
      matches(instruction, 0x3f20'0c00U, 0x1d00'0800U))
    return load_store_simd_ordered;
  /* LDAPR; LDAPUR and STLUR to general registers. */
  if (matches(instruction, 0x3fff'fc00U, 0x38bf'c000U) ||
      matches(instruction, 0x3f20'0c00U, 0x1900'0000U))
    return load_store_rcpc;
  /* LDAPR and STLR with writeback, inside the ordered-pair allocation. */
  if (matches(instruction, 0x3fbf'fc00U, 0x1980'0800U))
    return load_store_ordered_writeback;
  /* LDIAPP, STILP, and the FEAT_LSCP pair forms. */
  if (matches(instruction, 0x3f20'0c00U, 0x1900'0800U))
    return load_store_ordered_pair;

  if (load_store_64b_form(instruction))
    return load_store_64b;
  if (matches(instruction, 0x3b20'0c00U, 0x1900'0400U)) // CPY*, SET*
    return memory_copy_set;
  if (arm64::detail::decode_atomic_floating(instruction).recognized)
    return load_store_atomic_floating;

  /* Literal loads, except the non-faulting literal PRFM hint. */
  if (matches(instruction, 0x3b00'0000U, 0x1800'0000U) &&
      !matches(instruction, 0xff00'0000U, 0xd800'0000U))
    return load_literal;
  if (matches(instruction, 0x3b00'0000U, 0x3900'0000U))
    return load_store_unsigned;
  /* Unscaled, post-index, unprivileged, and pre-index forms, and the
   * register-offset form. The other register-offset cells hold atomics,
   * LDAPR, LS64, and LDRAA/LDRAB. */
  if (matches(instruction, 0x3b00'0000U, 0x3800'0000U) &&
      ((instruction & 0x0020'0000U) == 0U ||
       ((instruction >> 10) & 3U) == 2U))
    return load_store_other;
  return nullptr;
}

Handler exclusive_handler(std::uint32_t instruction) noexcept {
  /* Read-check-write shares its allocations with the scalar atomics and
   * FEAT_LSE128. */
  if (read_check_write_pair(instruction))
    return load_store_rcw_pair;
  if (read_check_write_scalar(instruction))
    return load_store_rcw;
  if (matches(instruction, 0xbf20'0c00U, 0x1920'0000U))
    return load_store_atomic_pair_128;
  /* LSE and FEAT_LSUI atomic memory operations. LDAPR and LS64 share the LSE
   * allocation, and the unimplemented FEAT_MTE tag-memory cells share the
   * size-3 LSUI cells; none of them reaches this helper. */
  if ((matches(instruction, 0x3f20'0c00U, 0x3820'0000U) &&
       !matches(instruction, 0x3fff'fc00U, 0x38bf'c000U) &&
       !load_store_64b_form(instruction)) ||
      (matches(instruction, 0x3f20'0c00U, 0x1920'0400U) &&
       !matches(instruction, 0xff20'0000U, 0xd920'0000U)))
    return load_store_atomic;
  /* CASP, CASPT. */
  if (matches(instruction, 0xbfa0'0000U, 0x0820'0000U) ||
      matches(instruction, 0xbfa0'0000U, 0x0980'0000U))
    return load_store_compare_swap_pair;
  /* CAS, CAST. */
  if (matches(instruction, 0x3fa0'0000U, 0x08a0'0000U) ||
      matches(instruction, 0xbfa0'0000U, 0x8980'0000U))
    return load_store_compare_swap;
  /* LDXR and STXR families, their unprivileged forms, LDXP and STXP. */
  if (matches(instruction, 0x3fa0'0000U, 0x0800'0000U) ||
      matches(instruction, 0xbfa0'0000U, 0x8900'0000U) ||
      matches(instruction, 0xbfa0'0000U, 0x8820'0000U))
    return load_store_exclusive;
  return nullptr;
}

Handler ordered_store_handler(std::uint32_t instruction) noexcept {
  /* STLR and STLLR: the non-exclusive, non-pair ordered store cells. */
  if (matches(instruction, 0x3fe0'0000U, 0x0880'0000U))
    return load_store_exclusive;
  return nullptr;
}

} // namespace uml::cpu::arm64::jit::semantic
