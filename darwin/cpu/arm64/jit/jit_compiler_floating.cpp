// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_compiler_families.hpp"

#include "cpu/arm64/detail/bfloat_conversion.hpp"
#include "cpu/arm64/detail/floating_arithmetic.hpp"
#include "cpu/arm64/detail/floating_conversion.hpp"
#include "cpu/arm64/detail/floating_fused.hpp"
#include "cpu/arm64/detail/floating_horizontal.hpp"
#include "cpu/arm64/detail/floating_integer_conversion.hpp"
#include "cpu/arm64/detail/floating_reciprocal.hpp"
#include "cpu/arm64/detail/floating_round.hpp"
#include "cpu/arm64/floating_controls.hpp"
#include "cpu/host_capabilities.hpp"

#include <cstddef>

namespace uml::cpu::arm64::jit {
namespace {

/* Under FPCR.NEP a scalar result keeps the other bits of the register the
 * ASL names as its merge source. Every source is read whole into its host
 * register, so the native instruction merges correctly from a source (V[n],
 * V[a], V[m]); only the forms that merge into the old destination need it
 * loaded first. FPCR.NEP is guest state only where the virtual CPU implements
 * FEAT_AFP, so elsewhere no form reads its destination. */
[[nodiscard]] bool nep_merges_into_destination() noexcept
{
  return implements_afp(host_capabilities());
}

} // namespace

[[nodiscard]] bool
JitCompilerFamilies::emit_basic_floating(std::uint32_t instruction) {
  const auto decoded =
      arm64::detail::decode_basic_floating_arithmetic(instruction);
  if (!decoded.recognized || !decoded.valid)
    return false;
  const auto &host = host_capabilities();
  if (!host.floating_point || (!decoded.scalar && !host.advanced_simd) ||
      (decoded.width == 16U && !host.fp16))
    return false;
  const unsigned destination = instruction & 31U;
  const unsigned left = (instruction >> 5) & 31U;
  const unsigned right = (instruction >> 16) & 31U;
  /*
   * Scalar FSQRT merges into its old destination under FPCR.NEP; the binary
   * forms merge into their first source, and vector forms overwrite or
   * architecturally zero every destination bit outside their active lanes.
   */
  const auto native_left = read_native_vector(left, 1);
  const auto native_right =
      decoded.unary
          ? NativeVector{}
          : right == left ? native_left
                          : read_native_vector(right, 2);
  const auto native_destination = write_native_vector(
      destination,
      decoded.scalar && decoded.unary && nep_merges_into_destination(), 0);
  auto native = instruction & ~(31U | (31U << 5));
  native |= native_destination.host | (native_left.host << 5);
  if (!decoded.unary) {
    native &= ~(31U << 16);
    native |= native_right.host << 16;
  }
  emit(native);
  commit_native_vector(native_destination, destination);
  mark_native_fp();
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_fused_floating(std::uint32_t instruction) {
  const auto decoded =
      arm64::detail::decode_fused_floating_arithmetic(instruction);
  if (!decoded.recognized || !decoded.valid ||
      !host_capabilities().floating_point ||
      (!decoded.scalar && !host_capabilities().advanced_simd) ||
      (decoded.width == 16U && !host_capabilities().fp16))
    return false;
  const unsigned destination = instruction & 31U;
  const unsigned left = (instruction >> 5) & 31U;
  const unsigned right = (instruction >> 16) & 31U;
  const auto native_left = read_native_vector(left, 0);
  const auto native_right =
      right == left ? native_left : read_native_vector(right, 1);
  auto native = instruction & ~((31U << 16) | (31U << 5) | 31U);
  native |= (native_right.host << 16) | (native_left.host << 5);
  if (decoded.scalar) {
    const unsigned addend = (instruction >> 10) & 31U;
    const auto native_addend =
        addend == left
            ? native_left
            : addend == right
                  ? native_right
                  : read_native_vector(addend, 2);
    const auto native_destination =
        write_native_vector(destination, false, 3);
    native &= ~(31U << 10);
    native |= (native_addend.host << 10) | native_destination.host;
    emit(native);
    commit_native_vector(native_destination, destination);
  } else {
    const auto native_destination =
        write_native_vector(destination, true, 2);
    native |= native_destination.host;
    emit(native);
    commit_native_vector(native_destination, destination);
  }
  mark_native_fp();
  return true;
}

[[nodiscard]] bool JitCompilerFamilies::emit_simd_integer_float_convert(
    std::uint32_t instruction) {
  /* Vector forms leave Q free; scalar forms are op0 01x1. */
  const auto vector_operation = instruction & 0xbfbf'fc00U;
  const auto scalar_operation = instruction & 0xffbf'fc00U;
  const bool vector = vector_operation == 0x0e21'd800U ||
                      vector_operation == 0x2e21'd800U ||
                      vector_operation == 0x0ea1'b800U ||
                      vector_operation == 0x2ea1'b800U;
  const bool scalar = scalar_operation == 0x5e21'd800U ||
                      scalar_operation == 0x7e21'd800U ||
                      scalar_operation == 0x5ea1'b800U ||
                      scalar_operation == 0x7ea1'b800U;
  if (!vector && !scalar)
    return false;
  const bool is_double = (instruction & 0x0040'0000U) != 0;
  const bool q = (instruction & 0x4000'0000U) != 0;
  if ((is_double && !q && !scalar) || !host_capabilities().floating_point ||
      !host_capabilities().advanced_simd)
    return false;
  emit_native_vector_unary(instruction,
                           scalar && nep_merges_into_destination());
  mark_native_fp();
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_integer_to_floating(std::uint32_t instruction) {
  const auto decoded = arm64::detail::decode_integer_to_floating(instruction);
  if (!decoded.recognized || !decoded.valid)
    return false;
  const auto &host = host_capabilities();
  if ((decoded.source_gpr && !host.floating_point) ||
      (!decoded.source_gpr && !host.advanced_simd) ||
      (decoded.destination_width == 16U && !host.fp16))
    return false;
  if (decoded.source_gpr) {
    const unsigned destination = instruction & 31U;
    const unsigned source = (instruction >> 5) & 31U;
    const auto native_source =
        read_native_gpr(source, false, 8);
    const auto native_destination = write_native_vector(
        destination, decoded.scalar && nep_merges_into_destination(), 0);
    emit((instruction & ~0x3ffU) | (native_source.host << 5) |
         native_destination.host);
    commit_native_vector(native_destination, destination);
  } else {
    /*
     * The helper imports Rn before defining Rd. This ordering is
     * architecturally relevant when Rd == Rn and the zeroing form does not
     * otherwise need an old destination live-in.
     */
    emit_native_vector_unary(
        instruction, decoded.scalar && nep_merges_into_destination());
  }
  mark_native_fp();
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_floating_to_integer(std::uint32_t instruction) {
  const auto decoded = arm64::detail::decode_floating_to_integer(instruction);
  if (!decoded.recognized || !decoded.valid)
    return false;
  const auto &host = host_capabilities();
  if ((decoded.destination_gpr && !host.floating_point) ||
      (!decoded.destination_gpr && !host.advanced_simd) ||
      (decoded.source_width == 16U && !host.fp16))
    return false;
  const unsigned destination = instruction & 31U;
  const unsigned source = (instruction >> 5) & 31U;
  const auto native_source = read_native_vector(source, 1);
  if (decoded.destination_gpr) {
    const auto native_destination =
        write_native_gpr(destination, false, false, 8);
    emit((instruction & ~0x3ffU) | (native_source.host << 5) |
         native_destination.host);
    commit_native_gpr(native_destination, destination, false);
  } else {
    const auto native_destination = write_native_vector(
        destination, decoded.scalar && nep_merges_into_destination(), 0);
    emit((instruction & ~0x3ffU) | (native_source.host << 5) |
         native_destination.host);
    commit_native_vector(native_destination, destination);
  }
  mark_native_fp();
  return true;
}

[[nodiscard]] bool JitCompilerFamilies::emit_floating_javascript_convert(
    std::uint32_t instruction) {
  if ((instruction & 0xffff'fc00U) != 0x1e7e'0000U ||
      !host_capabilities().has(ArmFeature::jscvt))
    return false;
  const unsigned destination = instruction & 31U;
  const unsigned source = (instruction >> 5) & 31U;
  const auto native_source = read_native_vector(source, 1);
  const auto native_destination =
      write_native_gpr(destination, false, false, 8);
  emit((instruction & ~0x3ffU) | (native_source.host << 5) |
       native_destination.host);
  if (!host_capabilities().jscvt_subnormal_flags_correct) {
    /*
     * Translation-time host characterization keeps unaffected machines at
     * one native semantic word. Affected hosts retain the contained status
     * correction without an engine fallback or a runtime feature check.
     */
    const auto indicator_done = make_label();
    emit(0x9e66'000aU |
         (native_source.host << 5)); // fmov x10, d<native source>
    emit(0xd374'f94bU); // ubfx x11, x10, #52, #11
    compare_zero(indicator_done, 11U, true, true);
    emit(0xd374'cd4bU); // lsl x11, x10, #12
    compare_zero(indicator_done, 11U, true, false);
    emit(0xd51b'421fU); // msr nzcv, xzr
    bind(indicator_done);
  }
  commit_native_gpr(native_destination, destination, false);
  store_flags();
  mark_native_fp();
  return true;
}

[[nodiscard]] bool JitCompilerFamilies::emit_floating_precision_convert(
    std::uint32_t instruction) {
  const auto reduced =
      arm64::detail::decode_bfloat_conversion(instruction);
  if (reduced.recognized) {
    if (!host_capabilities().has(ArmFeature::bf16))
      return false;
    emit_native_vector_unary(
        instruction,
        reduced.upper || (reduced.scalar && nep_merges_into_destination()));
    mark_native_fp();
    return true;
  }
  const auto vector =
      arm64::detail::decode_simd_floating_precision_vector(instruction);
  if (vector.recognized) {
    const auto &host = host_capabilities();
    if (!vector.valid || !host.advanced_simd)
      return false;
    /*
     * Narrowing upper forms preserve the lower half. Widening upper forms
     * merely select the upper source half and overwrite the destination.
     * Scalar FCVTXN merges into the old destination under FPCR.NEP.
     */
    emit_native_vector_unary(
        instruction, (vector.narrow && vector.upper) ||
                         (vector.scalar && nep_merges_into_destination()));
    mark_native_fp();
    return true;
  }
  if ((instruction & 0xff20'7c00U) != 0x1e20'4000U)
    return false;
  const unsigned operation = (instruction >> 15) & 63U;
  if (operation < 4U || operation > 7U)
    return false;
  const unsigned source_type = (instruction >> 22) & 3U;
  const unsigned destination_type = (instruction >> 15) & 3U;
  if (source_type == 2U || destination_type == 2U ||
      source_type == destination_type || !host_capabilities().floating_point ||
      ((source_type == 3U || destination_type == 3U) &&
       !host_capabilities().fp16))
    return false;
  emit_native_vector_unary(instruction, nep_merges_into_destination());
  mark_native_fp();
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_floating_round(std::uint32_t instruction) {
  const auto decoded = arm64::detail::decode_floating_round(instruction);
  const auto &host = host_capabilities();
  if (!decoded.recognized || !decoded.valid || !host.floating_point ||
      (!decoded.scalar && !host.advanced_simd) ||
      (decoded.width == 16U && !host.fp16) ||
      (arm64::detail::is_sized_round(decoded.operation) &&
       !host.has(ArmFeature::frintts)))
    return false;
  emit_native_vector_unary(instruction,
                           decoded.scalar && nep_merges_into_destination());
  mark_native_fp();
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_floating_unary(std::uint32_t instruction) {
  const auto &host = host_capabilities();
  if (!host.floating_point)
    return false;
  bool valid = false;
  bool vector = false;
  bool fp16 = false;
  if ((instruction & 0xff20'7c00U) == 0x1e20'4000U) {
    const unsigned operation = (instruction >> 15) & 63U;
    const unsigned type = (instruction >> 22) & 3U;
    valid = (operation == 1U || operation == 2U) && type != 2U;
    fp16 = type == 3U;
  } else {
    vector = true;
    const bool q = (instruction & 0x4000'0000U) != 0;
    const auto half_operation = instruction & 0xbfff'fc00U;
    const auto ordinary_operation = instruction & 0xbf3f'fc00U;
    if (half_operation == 0x0ef8'f800U || half_operation == 0x2ef8'f800U) {
      valid = true;
      fp16 = true;
    } else if (ordinary_operation == 0x0e20'f800U ||
               ordinary_operation == 0x2e20'f800U) {
      const bool is_double = (instruction & 0x0040'0000U) != 0;
      valid = (instruction & 0x0080'0000U) != 0 && (!is_double || q);
    } else if (ordinary_operation == 0x2e21'f800U) {
      const bool is_double = (instruction & 0x0040'0000U) != 0;
      valid = (instruction & 0x0080'0000U) != 0 && (!is_double || q);
    }
  }
  if (!valid || (vector && !host.advanced_simd) || (fp16 && !host.fp16))
    return false;
  /* Scalar FABS and FNEG merge into the old destination under FPCR.NEP. */
  emit_native_vector_unary(instruction,
                           !vector && nep_merges_into_destination());
  mark_native_fp();
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_floating_horizontal(std::uint32_t instruction) {
  const auto decoded = arm64::detail::decode_floating_horizontal(instruction);
  const auto &host = host_capabilities();
  const bool scalar_fp =
      decoded.form ==
          arm64::detail::FloatingHorizontalForm::scalar_binary &&
      decoded.operation !=
          arm64::detail::FloatingHorizontalOperation::absolute_difference;
  if (!decoded.recognized || !decoded.valid ||
      (scalar_fp ? !host.floating_point : !host.advanced_simd) ||
      (decoded.width == 16U && !host.fp16))
    return false;

  const unsigned destination = instruction & 31U;
  const unsigned left = (instruction >> 5) & 31U;
  const auto native_left = read_native_vector(left, 1);
  auto native =
      (instruction & ~(31U | (31U << 5))) | (native_left.host << 5);
  if (decoded.form == arm64::detail::FloatingHorizontalForm::scalar_binary ||
      decoded.form == arm64::detail::FloatingHorizontalForm::vector_binary ||
      decoded.form == arm64::detail::FloatingHorizontalForm::vector_pairwise) {
    const unsigned right = (instruction >> 16) & 31U;
    const auto native_right =
        right == left ? native_left : read_native_vector(right, 2);
    native =
        (native & ~(31U << 16)) | (native_right.host << 16);
  }
  const auto native_destination =
      write_native_vector(destination, false, 0);
  native |= native_destination.host;
  emit(native);
  commit_native_vector(native_destination, destination);
  mark_native_fp();
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_floating_reciprocal(std::uint32_t instruction) {
  const auto decoded =
      arm64::detail::decode_floating_reciprocal(instruction);
  const auto &host = host_capabilities();
  if (!decoded.recognized || !decoded.valid || !host.advanced_simd ||
      (decoded.width == 16U && !host.fp16))
    return false;

  const unsigned destination = instruction & 31U;
  const unsigned first = (instruction >> 5) & 31U;
  const unsigned second = (instruction >> 16) & 31U;

  /*
   * Scalar unary forms merge into the old destination under FPCR.NEP, scalar
   * binary forms into their first source; vector forms never read the old
   * destination.
   */
  const auto native_first = read_native_vector(first, 1);
  const auto native_second =
      decoded.unary
          ? NativeVector{}
          : second == first ? native_first
                            : read_native_vector(second, 2);
  const auto native_destination = write_native_vector(
      destination,
      decoded.scalar && decoded.unary && nep_merges_into_destination(), 0);
  auto native = (instruction & ~(31U | (31U << 5))) |
                native_destination.host | (native_first.host << 5);
  if (!decoded.unary) {
    native &= ~(31U << 16);
    native |= native_second.host << 16;
  }
  emit(native);
  commit_native_vector(native_destination, destination);
  mark_native_fp();
  return true;
}

[[nodiscard]] bool JitCompilerFamilies::host_supports_floating_type(
    std::uint32_t instruction) const {
  const unsigned type = (instruction >> 22) & 3U;
  return type != 2U && host_capabilities().floating_point &&
         (type != 3U || host_capabilities().fp16);
}

void JitCompilerFamilies::emit_floating_compare(std::uint32_t instruction,
                                                bool conditional) {
  const unsigned left = (instruction >> 5) & 31U;
  const unsigned right = (instruction >> 16) & 31U;
  const auto native_left = read_native_vector(left, 0);
  if (conditional)
    load_flags();
  auto native =
      (instruction & ~(31U << 5)) | (native_left.host << 5);
  if (conditional || (instruction & 8U) == 0U) {
    const auto native_right =
        right == left ? native_left : read_native_vector(right, 1);
    native = (native & ~(31U << 16)) | (native_right.host << 16);
  }
  emit(native);
  store_flags();
  mark_native_fp();
}

void JitCompilerFamilies::emit_floating_select(std::uint32_t instruction) {
  const unsigned destination = instruction & 31U;
  const unsigned left = (instruction >> 5) & 31U;
  const unsigned right = (instruction >> 16) & 31U;
  const auto native_left = read_native_vector(left, 1);
  const auto native_right =
      right == left ? native_left : read_native_vector(right, 2);
  const auto native_destination =
      write_native_vector(destination, false, 0);
  load_flags();
  emit((instruction & ~((31U << 16) | (31U << 5) | 31U)) |
       (native_right.host << 16) | (native_left.host << 5) |
       native_destination.host);
  commit_native_vector(native_destination, destination);
}

void JitCompilerFamilies::emit_floating_move(std::uint32_t instruction,
                                             bool immediate) {
  const unsigned destination = instruction & 31U;
  auto native = instruction & ~31U;
  if (!immediate) {
    const unsigned source = (instruction >> 5) & 31U;
    const auto native_source = read_native_vector(source, 1);
    native = (native & ~(31U << 5)) | (native_source.host << 5);
  }
  const auto native_destination =
      write_native_vector(destination, false, 0);
  native |= native_destination.host;
  emit(native);
  commit_native_vector(native_destination, destination);
}

} // namespace uml::cpu::arm64::jit
