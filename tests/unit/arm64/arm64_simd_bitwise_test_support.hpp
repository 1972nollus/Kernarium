// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_simd_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_simd_bitwise_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

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

} // namespace arm64_simd_bitwise_test
