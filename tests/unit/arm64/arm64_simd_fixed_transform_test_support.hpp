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

struct Registers4 final {
  unsigned destination;
  unsigned first;
  unsigned second;
  unsigned third;
};

inline constexpr std::array alias2{
    Registers2{28, 29},
    Registers2{28, 28},
};

/* All set partitions of the three architectural register roles. */
inline constexpr std::array alias3{
    Registers3{28, 29, 30}, Registers3{28, 28, 29}, Registers3{28, 29, 28},
    Registers3{28, 29, 29}, Registers3{28, 28, 28},
};

/* All set partitions of the four architectural register roles. */
inline constexpr std::array alias4{
    Registers4{28, 29, 30, 31}, Registers4{28, 28, 29, 30},
    Registers4{28, 29, 28, 30}, Registers4{28, 29, 30, 28},
    Registers4{28, 29, 29, 30}, Registers4{28, 29, 30, 29},
    Registers4{28, 29, 30, 30}, Registers4{28, 28, 29, 29},
    Registers4{28, 29, 28, 29}, Registers4{28, 29, 29, 28},
    Registers4{28, 28, 28, 29}, Registers4{28, 28, 29, 28},
    Registers4{28, 29, 28, 28}, Registers4{28, 29, 29, 29},
    Registers4{28, 28, 28, 28},
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

[[nodiscard]] inline arm64::CpuState state(std::uint64_t seed) {
  arm64::CpuState cpu{};
  for (unsigned index = 0; index < cpu.registers.size(); ++index)
    cpu.registers[index] = seed ^ (0x8d00'0000'4000'0000ULL |
                                   (static_cast<std::uint64_t>(index) << 17) |
                                   index * 0x0101'0307ULL);
  cpu.sp = (seed ^ 0x6a09'e667'f3bc'c908ULL) & ~std::uint64_t{15};
  cpu.nzcv = 0xa000'0000U;
  cpu.fpcr = 0x0040'0000U | (static_cast<std::uint32_t>(seed & 3U) << 22);
  cpu.fpsr = 0x0800'0000U | (static_cast<std::uint32_t>(seed) & 0x9fU);
  cpu.fpmr = seed ^ 0x0000'0000'0012'0041ULL;
  cpu.tls = seed ^ 0xbb67'ae85'84ca'a73bULL;
  for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
    for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
      cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
          seed + reg * 61U + byte * 37U + (reg ^ byte) * 11U);
  return cpu;
}

[[nodiscard]] inline std::array<std::uint32_t, 4>
words32(const arm64::VectorRegister &source) {
  std::array<std::uint32_t, 4> result{};
  for (unsigned lane = 0; lane < result.size(); ++lane)
    for (unsigned byte = 0; byte < 4U; ++byte)
      result[lane] |=
          std::to_integer<std::uint32_t>(source.bytes[lane * 4U + byte])
          << (byte * 8U);
  return result;
}

inline void set_words32(arm64::VectorRegister &destination,
                        const std::array<std::uint32_t, 4> &values) {
  for (unsigned lane = 0; lane < values.size(); ++lane)
    for (unsigned byte = 0; byte < 4U; ++byte)
      destination.bytes[lane * 4U + byte] =
          static_cast<std::byte>(values[lane] >> (byte * 8U));
}

[[nodiscard]] inline std::array<std::uint64_t, 2>
words64(const arm64::VectorRegister &source) {
  std::array<std::uint64_t, 2> result{};
  for (unsigned lane = 0; lane < result.size(); ++lane)
    for (unsigned byte = 0; byte < 8U; ++byte)
      result[lane] |=
          std::to_integer<std::uint64_t>(source.bytes[lane * 8U + byte])
          << (byte * 8U);
  return result;
}

inline void set_words64(arm64::VectorRegister &destination,
                        const std::array<std::uint64_t, 2> &values) {
  for (unsigned lane = 0; lane < values.size(); ++lane)
    for (unsigned byte = 0; byte < 8U; ++byte)
      destination.bytes[lane * 8U + byte] =
          static_cast<std::byte>(values[lane] >> (byte * 8U));
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

[[nodiscard]] inline engine::NativeRequirements feature3() {
  return engine::NativeRequirements::features(::uml::cpu::ArmFeature::sm3);
}

[[nodiscard]] inline engine::NativeRequirements feature4() {
  return engine::NativeRequirements::features(::uml::cpu::ArmFeature::sm4);
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

inline constexpr std::array transform3_three_bases{
    0xce60'c000U,
    0xce60'c400U,
};

[[nodiscard]] constexpr std::uint32_t transform3_three(unsigned operation,
                                                       Registers3 registers) {
  return transform3_three_bases[operation] | ((registers.second & 31U) << 16) |
         ((registers.first & 31U) << 5) | (registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t transform3_four(Registers4 registers) {
  return 0xce40'0000U | ((registers.second & 31U) << 16) |
         ((registers.third & 31U) << 10) | ((registers.first & 31U) << 5) |
         (registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
transform3_indexed(unsigned operation, unsigned index, Registers3 registers) {
  return 0xce40'8000U | ((registers.second & 31U) << 16) |
         ((index & 3U) << 12) | ((operation & 3U) << 10) |
         ((registers.first & 31U) << 5) | (registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t transform4_three(Registers3 registers) {
  return 0xce60'c800U | ((registers.second & 31U) << 16) |
         ((registers.first & 31U) << 5) | (registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t transform4_two(Registers2 registers) {
  return 0xcec0'8400U | ((registers.source & 31U) << 5) |
         (registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t choose32(std::uint32_t x, std::uint32_t y,
                                               std::uint32_t z) {
  return ((y ^ z) & x) ^ z;
}

[[nodiscard]] constexpr std::uint32_t
majority32(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
  return (x & y) | ((x | y) & z);
}

[[nodiscard]] inline std::array<std::uint32_t, 4>
rounds32_first(std::array<std::uint32_t, 4> x, std::uint32_t y,
               const std::array<std::uint32_t, 4> &schedule,
               unsigned operation) {
  for (unsigned round = 0; round < 4U; ++round) {
    const auto function = operation == 0U   ? choose32(x[1], x[2], x[3])
                          : operation == 1U ? x[1] ^ x[2] ^ x[3]
                                            : majority32(x[1], x[2], x[3]);
    const std::uint32_t next =
        y + std::rotl(x[0], 5) + function + schedule[round];
    x[1] = std::rotl(x[1], 30);
    y = x[3];
    x = {next, x[0], x[1], x[2]};
  }
  return x;
}

[[nodiscard]] inline std::array<std::uint32_t, 4>
rounds32_second(std::array<std::uint32_t, 4> x, std::array<std::uint32_t, 4> y,
                const std::array<std::uint32_t, 4> &schedule, bool first_part) {
  for (unsigned round = 0; round < 4U; ++round) {
    const auto sigma0 =
        std::rotr(x[0], 2) ^ std::rotr(x[0], 13) ^ std::rotr(x[0], 22);
    const auto sigma1 =
        std::rotr(y[0], 6) ^ std::rotr(y[0], 11) ^ std::rotr(y[0], 25);
    const std::uint32_t temporary =
        y[3] + sigma1 + choose32(y[0], y[1], y[2]) + schedule[round];
    x[3] += temporary;
    y[3] = temporary + sigma0 + majority32(x[0], x[1], x[2]);
    const auto old_x = x;
    const auto old_y = y;
    x = {old_y[3], old_x[0], old_x[1], old_x[2]};
    y = {old_x[3], old_y[0], old_y[1], old_y[2]};
  }
  return first_part ? x : y;
}

[[nodiscard]] inline std::array<std::uint32_t, 4>
oracle_transform32_three(const arm64::CpuState &initial, unsigned operation,
                         Registers3 registers) {
  const auto destination = words32(initial.vectors[registers.destination]);
  const auto first = words32(initial.vectors[registers.first]);
  const auto second = words32(initial.vectors[registers.second]);
  if (operation < 3U)
    return rounds32_first(destination, first[0], second, operation);
  if (operation == 3U)
    return {
        destination[2] ^ destination[0] ^ second[0],
        destination[3] ^ destination[1] ^ second[1],
        first[0] ^ destination[2] ^ second[2],
        first[1] ^ destination[3] ^ second[3],
    };
  if (operation == 4U)
    return rounds32_second(destination, first, second, true);
  if (operation == 5U)
    return rounds32_second(first, destination, second, false);

  const std::array temporary{
      first[1],
      first[2],
      first[3],
      second[0],
  };
  std::array<std::uint32_t, 4> result{};
  for (unsigned lane = 0; lane < 2U; ++lane) {
    const auto value = second[lane + 2U];
    result[lane] =
        destination[lane] + temporary[lane] +
        (std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10));
  }
  for (unsigned lane = 2U; lane < result.size(); ++lane) {
    const auto value = result[lane - 2U];
    result[lane] =
        destination[lane] + temporary[lane] +
        (std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10));
  }
  return result;
}

[[nodiscard]] inline std::array<std::uint32_t, 4>
oracle_transform32_two(const arm64::CpuState &initial, unsigned operation,
                       Registers2 registers) {
  const auto destination = words32(initial.vectors[registers.destination]);
  const auto source = words32(initial.vectors[registers.source]);
  if (operation == 0U)
    return {std::rotl(source[0], 30), 0, 0, 0};
  if (operation == 1U) {
    const std::array temporary{
        destination[0] ^ source[1],
        destination[1] ^ source[2],
        destination[2] ^ source[3],
        destination[3],
    };
    return {
        std::rotl(temporary[0], 1),
        std::rotl(temporary[1], 1),
        std::rotl(temporary[2], 1),
        std::rotl(temporary[3], 1) ^ std::rotl(temporary[0], 2),
    };
  }
  const std::array temporary{
      destination[1],
      destination[2],
      destination[3],
      source[0],
  };
  std::array<std::uint32_t, 4> result{};
  for (unsigned lane = 0; lane < result.size(); ++lane) {
    const auto value = temporary[lane];
    result[lane] = destination[lane] +
                   (std::rotr(value, 7) ^ std::rotr(value, 18) ^ (value >> 3));
  }
  return result;
}

[[nodiscard]] inline std::array<std::uint64_t, 2>
oracle_transform64_three(const arm64::CpuState &initial, unsigned operation,
                         Registers3 registers) {
  const auto destination = words64(initial.vectors[registers.destination]);
  const auto first = words64(initial.vectors[registers.first]);
  const auto second = words64(initial.vectors[registers.second]);
  std::array<std::uint64_t, 2> result{};
  if (operation == 0U) {
    const auto sigma1 = [](std::uint64_t value) {
      return std::rotr(value, 14) ^ std::rotr(value, 18) ^ std::rotr(value, 41);
    };
    result[1] = ((second[1] & first[0]) ^ (~second[1] & first[1])) +
                sigma1(second[1]) + destination[1];
    const auto temporary = result[1] + second[0];
    result[0] = ((temporary & second[1]) ^ (~temporary & first[0])) +
                sigma1(temporary) + destination[0];
  } else if (operation == 1U) {
    const auto sigma0 = [](std::uint64_t value) {
      return std::rotr(value, 28) ^ std::rotr(value, 34) ^ std::rotr(value, 39);
    };
    const auto majority = [](std::uint64_t a, std::uint64_t b,
                             std::uint64_t c) {
      return (a & b) | ((a | b) & c);
    };
    result[1] = majority(first[0], second[1], second[0]) + sigma0(second[0]) +
                destination[1];
    result[0] = majority(result[1], second[0], second[1]) + sigma0(result[1]) +
                destination[0];
  } else {
    for (unsigned lane = 0; lane < result.size(); ++lane) {
      const auto value = first[lane];
      result[lane] =
          destination[lane] + second[lane] +
          (std::rotr(value, 19) ^ std::rotr(value, 61) ^ (value >> 6));
    }
  }
  return result;
}

[[nodiscard]] inline std::array<std::uint64_t, 2>
oracle_transform64_two(const arm64::CpuState &initial, Registers2 registers) {
  const auto destination = words64(initial.vectors[registers.destination]);
  const auto source = words64(initial.vectors[registers.source]);
  const auto sigma0 = [](std::uint64_t value) {
    return std::rotr(value, 1) ^ std::rotr(value, 8) ^ (value >> 7);
  };
  return {
      destination[0] + sigma0(destination[1]),
      destination[1] + sigma0(source[0]),
  };
}

[[nodiscard]] inline std::array<std::uint32_t, 4>
oracle_transform3_three(const arm64::CpuState &initial, unsigned operation,
                        Registers3 registers) {
  const auto destination = words32(initial.vectors[registers.destination]);
  const auto first = words32(initial.vectors[registers.first]);
  const auto second = words32(initial.vectors[registers.second]);
  std::array<std::uint32_t, 4> result{};
  if (operation == 0U) {
    for (unsigned lane = 0; lane < 3U; ++lane) {
      const auto value =
          destination[lane] ^ first[lane] ^ std::rotl(second[lane + 1U], 15);
      result[lane] = value ^ std::rotl(value, 15) ^ std::rotl(value, 23);
    }
    const auto value = destination[3] ^ first[3] ^ std::rotl(result[0], 15);
    result[3] = value ^ std::rotl(value, 15) ^ std::rotl(value, 23);
    return result;
  }
  std::array<std::uint32_t, 4> temporary{};
  for (unsigned lane = 0; lane < result.size(); ++lane) {
    temporary[lane] = first[lane] ^ std::rotl(second[lane], 7);
    result[lane] = destination[lane] ^ temporary[lane];
  }
  auto correction = std::rotl(temporary[0], 15);
  correction ^= std::rotl(correction, 15) ^ std::rotl(correction, 23);
  result[3] ^= correction;
  return result;
}

[[nodiscard]] inline std::array<std::uint32_t, 4>
oracle_transform3_four(const arm64::CpuState &initial, Registers4 registers) {
  const auto first = words32(initial.vectors[registers.first]);
  const auto second = words32(initial.vectors[registers.second]);
  const auto third = words32(initial.vectors[registers.third]);
  return {
      0,
      0,
      0,
      std::rotl(std::rotl(first[3], 12) + second[3] + third[3], 7),
  };
}

[[nodiscard]] inline std::array<std::uint32_t, 4>
oracle_transform3_indexed(const arm64::CpuState &initial, unsigned operation,
                          unsigned index, Registers3 registers) {
  const auto destination = words32(initial.vectors[registers.destination]);
  const auto first = words32(initial.vectors[registers.first]);
  const auto second = words32(initial.vectors[registers.second]);
  std::uint32_t value{};
  if (operation == 0U || operation == 2U)
    value = destination[1] ^ destination[3] ^ destination[2];
  else if (operation == 1U)
    value = (destination[3] & destination[1]) |
            (destination[3] & destination[2]) |
            (destination[1] & destination[2]);
  else
    value =
        (destination[3] & destination[2]) | (~destination[3] & destination[1]);
  value +=
      destination[0] + second[index] +
      (operation < 2U ? first[3] ^ std::rotl(destination[3], 12) : first[3]);
  if (operation >= 2U)
    value ^= std::rotl(value, 9) ^ std::rotl(value, 17);
  return {
      destination[1],
      std::rotl(destination[2], operation < 2U ? 9 : 19),
      destination[3],
      value,
  };
}

inline constexpr std::array<std::uint8_t, 256> substitution_table{
    0xd6, 0x90, 0xe9, 0xfe, 0xcc, 0xe1, 0x3d, 0xb7, 0x16, 0xb6, 0x14, 0xc2,
    0x28, 0xfb, 0x2c, 0x05, 0x2b, 0x67, 0x9a, 0x76, 0x2a, 0xbe, 0x04, 0xc3,
    0xaa, 0x44, 0x13, 0x26, 0x49, 0x86, 0x06, 0x99, 0x9c, 0x42, 0x50, 0xf4,
    0x91, 0xef, 0x98, 0x7a, 0x33, 0x54, 0x0b, 0x43, 0xed, 0xcf, 0xac, 0x62,
    0xe4, 0xb3, 0x1c, 0xa9, 0xc9, 0x08, 0xe8, 0x95, 0x80, 0xdf, 0x94, 0xfa,
    0x75, 0x8f, 0x3f, 0xa6, 0x47, 0x07, 0xa7, 0xfc, 0xf3, 0x73, 0x17, 0xba,
    0x83, 0x59, 0x3c, 0x19, 0xe6, 0x85, 0x4f, 0xa8, 0x68, 0x6b, 0x81, 0xb2,
    0x71, 0x64, 0xda, 0x8b, 0xf8, 0xeb, 0x0f, 0x4b, 0x70, 0x56, 0x9d, 0x35,
    0x1e, 0x24, 0x0e, 0x5e, 0x63, 0x58, 0xd1, 0xa2, 0x25, 0x22, 0x7c, 0x3b,
    0x01, 0x21, 0x78, 0x87, 0xd4, 0x00, 0x46, 0x57, 0x9f, 0xd3, 0x27, 0x52,
    0x4c, 0x36, 0x02, 0xe7, 0xa0, 0xc4, 0xc8, 0x9e, 0xea, 0xbf, 0x8a, 0xd2,
    0x40, 0xc7, 0x38, 0xb5, 0xa3, 0xf7, 0xf2, 0xce, 0xf9, 0x61, 0x15, 0xa1,
    0xe0, 0xae, 0x5d, 0xa4, 0x9b, 0x34, 0x1a, 0x55, 0xad, 0x93, 0x32, 0x30,
    0xf5, 0x8c, 0xb1, 0xe3, 0x1d, 0xf6, 0xe2, 0x2e, 0x82, 0x66, 0xca, 0x60,
    0xc0, 0x29, 0x23, 0xab, 0x0d, 0x53, 0x4e, 0x6f, 0xd5, 0xdb, 0x37, 0x45,
    0xde, 0xfd, 0x8e, 0x2f, 0x03, 0xff, 0x6a, 0x72, 0x6d, 0x6c, 0x5b, 0x51,
    0x8d, 0x1b, 0xaf, 0x92, 0xbb, 0xdd, 0xbc, 0x7f, 0x11, 0xd9, 0x5c, 0x41,
    0x1f, 0x10, 0x5a, 0xd8, 0x0a, 0xc1, 0x31, 0x88, 0xa5, 0xcd, 0x7b, 0xbd,
    0x2d, 0x74, 0xd0, 0x12, 0xb8, 0xe5, 0xb4, 0xb0, 0x89, 0x69, 0x97, 0x4a,
    0x0c, 0x96, 0x77, 0x7e, 0x65, 0xb9, 0xf1, 0x09, 0xc5, 0x6e, 0xc6, 0x84,
    0x18, 0xf0, 0x7d, 0xec, 0x3a, 0xdc, 0x4d, 0x20, 0x79, 0xee, 0x5f, 0x3e,
    0xd7, 0xcb, 0x39, 0x48,
};

[[nodiscard]] inline std::uint32_t substitute(std::uint32_t value) {
  std::uint32_t result{};
  for (unsigned byte = 0; byte < 4U; ++byte)
    result |= static_cast<std::uint32_t>(
                  substitution_table[(value >> (byte * 8U)) & 0xffU])
              << (byte * 8U);
  return result;
}

[[nodiscard]] inline std::array<std::uint32_t, 4>
oracle_transform4_three(const arm64::CpuState &initial, Registers3 registers) {
  auto result = words32(initial.vectors[registers.first]);
  const auto constants = words32(initial.vectors[registers.second]);
  for (unsigned round = 0; round < 4U; ++round) {
    auto value = result[3] ^ result[2] ^ result[1] ^ constants[round];
    value = substitute(value);
    value ^= std::rotl(value, 13) ^ std::rotl(value, 23) ^ result[0];
    result = {result[1], result[2], result[3], value};
  }
  return result;
}

[[nodiscard]] inline std::array<std::uint32_t, 4>
oracle_transform4_two(const arm64::CpuState &initial, Registers2 registers) {
  auto result = words32(initial.vectors[registers.destination]);
  const auto keys = words32(initial.vectors[registers.source]);
  for (unsigned round = 0; round < 4U; ++round) {
    auto value = result[3] ^ result[2] ^ result[1] ^ keys[round];
    value = substitute(value);
    value ^= std::rotl(value, 2) ^ std::rotl(value, 10) ^ std::rotl(value, 18) ^
             std::rotl(value, 24) ^ result[0];
    result = {result[1], result[2], result[3], value};
  }
  return result;
}

inline void check_transform32_three(engine::EngineHarness &harness,
                                    std::string_view name, unsigned operation,
                                    Registers3 registers,
                                    const arm64::CpuState &initial) {
  auto expected = initial;
  set_words32(expected.vectors[registers.destination],
              oracle_transform32_three(initial, operation, registers));
  harness.check_one(name, transform32_three(operation, registers), initial,
                    expected, operation <= 3U ? feature1() : feature2());
}

inline void check_transform32_two(engine::EngineHarness &harness,
                                  std::string_view name, unsigned operation,
                                  Registers2 registers,
                                  const arm64::CpuState &initial) {
  auto expected = initial;
  set_words32(expected.vectors[registers.destination],
              oracle_transform32_two(initial, operation, registers));
  harness.check_one(name, transform32_two(operation, registers), initial,
                    expected, operation <= 1U ? feature1() : feature2());
}

inline void check_transform64_three(engine::EngineHarness &harness,
                                    std::string_view name, unsigned operation,
                                    Registers3 registers,
                                    const arm64::CpuState &initial) {
  auto expected = initial;
  set_words64(expected.vectors[registers.destination],
              oracle_transform64_three(initial, operation, registers));
  harness.check_one(name, transform64_three(operation, registers), initial,
                    expected, feature64());
}

inline void check_transform64_two(engine::EngineHarness &harness,
                                  std::string_view name, Registers2 registers,
                                  const arm64::CpuState &initial) {
  auto expected = initial;
  set_words64(expected.vectors[registers.destination],
              oracle_transform64_two(initial, registers));
  harness.check_one(name, transform64_two(registers), initial, expected,
                    feature64());
}

inline void check_transform3_three(engine::EngineHarness &harness,
                                   std::string_view name, unsigned operation,
                                   Registers3 registers,
                                   const arm64::CpuState &initial) {
  auto expected = initial;
  set_words32(expected.vectors[registers.destination],
              oracle_transform3_three(initial, operation, registers));
  harness.check_one(name, transform3_three(operation, registers), initial,
                    expected, feature3());
}

inline void check_transform3_four(engine::EngineHarness &harness,
                                  std::string_view name, Registers4 registers,
                                  const arm64::CpuState &initial) {
  auto expected = initial;
  set_words32(expected.vectors[registers.destination],
              oracle_transform3_four(initial, registers));
  harness.check_one(name, transform3_four(registers), initial, expected,
                    feature3());
}

inline void check_transform3_indexed(engine::EngineHarness &harness,
                                     std::string_view name, unsigned operation,
                                     unsigned index, Registers3 registers,
                                     const arm64::CpuState &initial) {
  auto expected = initial;
  set_words32(expected.vectors[registers.destination],
              oracle_transform3_indexed(initial, operation, index, registers));
  harness.check_one(name, transform3_indexed(operation, index, registers),
                    initial, expected, feature3());
}

inline void check_transform4_three(engine::EngineHarness &harness,
                                   std::string_view name, Registers3 registers,
                                   const arm64::CpuState &initial) {
  auto expected = initial;
  set_words32(expected.vectors[registers.destination],
              oracle_transform4_three(initial, registers));
  harness.check_one(name, transform4_three(registers), initial, expected,
                    feature4());
}

inline void check_transform4_two(engine::EngineHarness &harness,
                                 std::string_view name, Registers2 registers,
                                 const arm64::CpuState &initial) {
  auto expected = initial;
  set_words32(expected.vectors[registers.destination],
              oracle_transform4_two(initial, registers));
  harness.check_one(name, transform4_two(registers), initial, expected,
                    feature4());
}

} // namespace arm64_simd_fixed_transform_test
