// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_bfloat_arithmetic_test_support.hpp"
#include "arm64_bfloat_dot_test_support.hpp"
#include "arm64_bfloat_matrix_test_support.hpp"
#include "arm64_bfloat_multiply_test_support.hpp"
#include "arm64_half_dot_test_support.hpp"
#include "arm64_half_fused_long_test_support.hpp"
#include "arm64_half_matrix_test_support.hpp"
#include "arm64_simd_addp_scalar_test_support.hpp"
#include "arm64_simd_dot_test_support.hpp"
#include "arm64_simd_fp_indexed_test_support.hpp"
#include "arm64_simd_immediate_test_support.hpp"
#include "arm64_simd_indexed_test_support.hpp"
#include "arm64_simd_lane_move_test_support.hpp"
#include "arm64_simd_lut_test_support.hpp"
#include "arm64_simd_matrix_test_support.hpp"
#include "arm64_simd_rearrange_test_support.hpp"
#include "arm64_simd_reduce_test_support.hpp"
#include "arm64_simd_shift_immediate_test_support.hpp"
#include "arm64_simd_shift_left_long_test_support.hpp"
#include "arm64_simd_widening_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

namespace {

[[nodiscard]] constexpr std::uint32_t
three_register(std::uint32_t operation, unsigned destination, unsigned left,
               unsigned right) noexcept {
  return operation | ((right & 31U) << 16U) | ((left & 31U) << 5U) |
         (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
move_wide(unsigned destination, unsigned immediate) noexcept {
  return 0xd280'0000U | ((immediate & 0xffffU) << 5U) |
         (destination & 31U);
}

enum class SimdBitwiseOperation : unsigned {
  bit_and,
  bit_clear,
  bit_or,
  or_not,
  bit_xor,
  select,
  insert_true,
  insert_false,
};

[[nodiscard]] constexpr std::uint32_t
simd_bitwise(SimdBitwiseOperation operation, unsigned destination,
             unsigned left, unsigned right) noexcept {
  const auto encoded = static_cast<unsigned>(operation);
  return 0x4e20'1c00U | ((encoded & 3U) << 22U) |
         ((encoded & 4U) << 27U) | ((right & 31U) << 16U) |
         ((left & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
simd_modified_immediate(unsigned immediate, unsigned destination) noexcept {
  return 0x4f00'0400U | ((immediate & 0xe0U) << 11U) | (14U << 12U) |
         ((immediate & 0x1fU) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
vector_unsigned_memory(bool load, unsigned byte_offset, unsigned base,
                       unsigned transfer) noexcept {
  return 0x3d00'0000U | ((load ? 3U : 2U) << 22U) |
         (((byte_offset / 16U) & 0xfffU) << 10U) |
         ((base & 31U) << 5U) | (transfer & 31U);
}

[[nodiscard]] constexpr std::uint32_t
atomic_floating(unsigned base, unsigned source, unsigned result) noexcept {
  /* Binary32 FADD, relaxed ordering, returning the old memory value. */
  return 0xbc20'0000U | ((source & 31U) << 16U) |
         ((base & 31U) << 5U) | (result & 31U);
}

[[nodiscard]] constexpr unsigned
simd_bitwise_byte(SimdBitwiseOperation operation, unsigned old_destination,
                  unsigned left, unsigned right) noexcept {
  switch (operation) {
  case SimdBitwiseOperation::bit_and:
    return left & right;
  case SimdBitwiseOperation::bit_clear:
    return left & (right ^ 0xffU);
  case SimdBitwiseOperation::bit_or:
    return left | right;
  case SimdBitwiseOperation::or_not:
    return left | (right ^ 0xffU);
  case SimdBitwiseOperation::bit_xor:
    return left ^ right;
  case SimdBitwiseOperation::select:
    return (old_destination & left) |
           ((old_destination ^ 0xffU) & right);
  case SimdBitwiseOperation::insert_true:
    return (old_destination & (right ^ 0xffU)) | (left & right);
  case SimdBitwiseOperation::insert_false:
    return (old_destination & right) | (left & (right ^ 0xffU));
  }
  return 0;
}

void apply_simd_bitwise(arm64::CpuState &state,
                        SimdBitwiseOperation operation, unsigned destination,
                        unsigned left, unsigned right) {
  const auto old_destination = state.vectors[destination].bytes;
  const auto left_value = state.vectors[left].bytes;
  const auto right_value = state.vectors[right].bytes;
  for (unsigned byte = 0; byte < 16; ++byte) {
    state.vectors[destination].bytes[byte] = static_cast<std::byte>(
        simd_bitwise_byte(
            operation, std::to_integer<unsigned>(old_destination[byte]),
            std::to_integer<unsigned>(left_value[byte]),
            std::to_integer<unsigned>(right_value[byte])));
  }
}

[[maybe_unused]] void seed_vector_transition_state(arm64::CpuState &state) {
  for (unsigned byte = 0; byte < 16; ++byte) {
    state.vectors[3].bytes[byte] =
        static_cast<std::byte>(0x19U + byte * 7U);
    state.vectors[4].bytes[byte] =
        static_cast<std::byte>(0xe3U - byte * 5U);
    state.vectors[5].bytes[byte] =
        static_cast<std::byte>(0x52U ^ (byte * 11U));
    state.vectors[7].bytes[byte] =
        static_cast<std::byte>(0xa7U - byte * 3U);
  }
}

void write_vector_u64(arm64::CpuState &state, unsigned reg, unsigned lane,
                      std::uint64_t value) {
  for (unsigned byte = 0; byte < 8; ++byte)
    state.vectors[reg].bytes[lane * 8U + byte] =
        static_cast<std::byte>(value >> (byte * 8U));
}

[[maybe_unused, nodiscard]] engine_tests::NativeRequirements
vector_memory_requirements() {
  return engine_tests::NativeRequirements::features({
      ::uml::cpu::ArmFeature::advanced_simd,
      ::uml::cpu::ArmFeature::floating_point,
  });
}

[[maybe_unused, nodiscard]] engine_tests::NativeRequirements
atomic_floating_requirements() {
  return engine_tests::NativeRequirements::features({
      ::uml::cpu::ArmFeature::advanced_simd,
      ::uml::cpu::ArmFeature::floating_point,
      ::uml::cpu::ArmFeature::lsfe,
  });
}

[[maybe_unused]] void ignore_counter_register(
    const arm64::CpuState &, const arm64::CpuState &expected_template,
    const arm64::CpuState &actual, std::string_view case_name,
    std::string_view engine, std::uint32_t instruction) {
  auto expected = expected_template;
  expected.registers[6] = actual.registers[6];
  engine_tests::require_architecture_equal(
      expected, actual, case_name, engine, instruction);
}


} // namespace
