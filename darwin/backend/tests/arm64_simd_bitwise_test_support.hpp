// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_simd_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_simd_bitwise_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

enum class Operation : unsigned {
  bit_and,
  bit_clear,
  bit_or,
  or_not,
  bit_xor,
  select,
  insert_true,
  insert_false,
};

struct Registers final {
  unsigned destination;
  unsigned left;
  unsigned right;
};

[[nodiscard]] constexpr std::uint32_t
instruction(bool q, Operation operation, Registers registers) noexcept {
  const auto encoded = static_cast<unsigned>(operation);
  return 0x0e20'1c00U | (q ? 0x4000'0000U : 0U) |
         ((encoded & 3U) << 22) | ((encoded & 4U) << 27) |
         ((registers.right & 31U) << 16) |
         ((registers.left & 31U) << 5) | (registers.destination & 31U);
}

[[nodiscard]] constexpr unsigned
result_byte(Operation operation, unsigned old_destination, unsigned left,
            unsigned right) noexcept {
  switch (operation) {
  case Operation::bit_and:
    return left & right;
  case Operation::bit_clear:
    return left & (right ^ 0xffU);
  case Operation::bit_or:
    return left | right;
  case Operation::or_not:
    return left | (right ^ 0xffU);
  case Operation::bit_xor:
    return left ^ right;
  case Operation::select:
    return (old_destination & left) |
           ((old_destination ^ 0xffU) & right);
  case Operation::insert_true:
    return (old_destination & (right ^ 0xffU)) | (left & right);
  case Operation::insert_false:
    return (old_destination & right) | (left & (right ^ 0xffU));
  }
  return 0;
}

[[nodiscard]] inline arm64::CpuState
expected_state(const arm64::CpuState &initial, bool q, Operation operation,
               Registers registers) {
  auto expected = initial;
  auto result = expected.vectors[registers.destination].bytes;
  const unsigned active_bytes = q ? 16U : 8U;
  for (unsigned byte = 0; byte < active_bytes; ++byte) {
    const auto old_destination = std::to_integer<unsigned>(
        initial.vectors[registers.destination].bytes[byte]);
    const auto left =
        std::to_integer<unsigned>(initial.vectors[registers.left].bytes[byte]);
    const auto right =
        std::to_integer<unsigned>(initial.vectors[registers.right].bytes[byte]);
    result[byte] = static_cast<std::byte>(
        result_byte(operation, old_destination, left, right));
  }
  for (unsigned byte = active_bytes; byte < 16U; ++byte)
    result[byte] = std::byte{};
  expected.vectors[registers.destination].bytes = result;
  expected.pc += 4;
  return expected;
}

class Harness final {
public:
  void check(std::string_view name, bool q, Operation operation,
             Registers registers, const arm64::CpuState &initial) {
    engines_.check_one(
        name, instruction(q, operation, registers), initial,
        expected_state(initial, q, operation, registers),
        engine_tests::NativeRequirements::features(
            uml::backend::ArmFeature::advanced_simd));
  }

  void print_summary(std::string_view suite) const {
    engines_.print_summary(suite);
  }

private:
  engine_tests::EngineHarness engines_;
};

} // namespace arm64_simd_bitwise_test
