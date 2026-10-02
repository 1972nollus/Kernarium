// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

namespace arm64_simd_fixed_transform_test {

namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;

struct Registers2 final {
  unsigned destination;
  unsigned source;
};

struct Registers3 final {
  unsigned destination;
  unsigned first;
  unsigned second;
};

[[noreturn]] inline void fail(std::string_view message) {
  std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()),
               message.data());
  std::abort();
}

inline void require(bool condition, std::string_view message) {
  if (!condition)
    fail(message);
}

[[nodiscard]] inline engine::NativeRequirements feature1() {
  return engine::NativeRequirements::features(::uml::cpu::ArmFeature::sha1);
}

[[nodiscard]] inline engine::NativeRequirements feature2() {
  return engine::NativeRequirements::features(::uml::cpu::ArmFeature::sha256);
}

[[nodiscard]] inline engine::NativeRequirements feature64() {
  return engine::NativeRequirements::features(::uml::cpu::ArmFeature::sha512);
}

[[nodiscard]] constexpr std::uint32_t transform32_three(unsigned operation,
                                                        Registers3 registers) {
  return 0x5e00'0000U | ((registers.second & 31U) << 16) |
         ((operation & 7U) << 12) | ((registers.first & 31U) << 5) |
         (registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t transform32_two(unsigned operation,
                                                      Registers2 registers) {
  return 0x5e28'0800U | ((operation & 3U) << 12) |
         ((registers.source & 31U) << 5) | (registers.destination & 31U);
}

inline constexpr std::array transform64_three_bases{
    0xce60'8000U,
    0xce60'8400U,
    0xce60'8800U,
};

[[nodiscard]] constexpr std::uint32_t transform64_three(unsigned operation,
                                                        Registers3 registers) {
  return transform64_three_bases[operation] | ((registers.second & 31U) << 16) |
         ((registers.first & 31U) << 5) | (registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t transform64_two(Registers2 registers) {
  return 0xcec0'8000U | ((registers.source & 31U) << 5) |
         (registers.destination & 31U);
}

} // namespace arm64_simd_fixed_transform_test
