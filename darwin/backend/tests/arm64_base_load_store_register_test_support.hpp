// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace arm64_load_store_register_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

inline constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr std::uint64_t data_base = engine_tests::engine_test_data_base;
inline constexpr std::uint64_t transfer_value = 0x0012'3456U;

struct IntegerVariant final {
  unsigned size_code;
  unsigned operation;
  std::size_t size;
  bool load;
  std::string_view name;
  std::string_view inventory_id;
};

struct Registers final {
  unsigned rm;
  unsigned rn;
  unsigned rt;
};

inline constexpr std::array integer_variants{
    IntegerVariant{0, 0, 1, false, "strb", "BASE-0426"},
    IntegerVariant{0, 1, 1, true, "ldrb", "BASE-0226"},
    IntegerVariant{0, 2, 1, true, "ldrsb-x", "BASE-0230"},
    IntegerVariant{0, 3, 1, true, "ldrsb-w", "BASE-0230"},
    IntegerVariant{1, 0, 2, false, "strh", "BASE-0428"},
    IntegerVariant{1, 1, 2, true, "ldrh", "BASE-0228"},
    IntegerVariant{1, 2, 2, true, "ldrsh-x", "BASE-0232"},
    IntegerVariant{1, 3, 2, true, "ldrsh-w", "BASE-0232"},
    IntegerVariant{2, 0, 4, false, "str-w", "BASE-0424"},
    IntegerVariant{2, 1, 4, true, "ldr-w", "BASE-0223"},
    IntegerVariant{2, 2, 4, true, "ldrsw-x", "BASE-0235"},
    IntegerVariant{3, 0, 8, false, "str-x", "BASE-0424"},
    IntegerVariant{3, 1, 8, true, "ldr-x", "BASE-0223"},
};

inline constexpr std::array legal_options{2U, 3U, 6U, 7U};
inline constexpr std::array illegal_options{0U, 1U, 4U, 5U};

[[nodiscard]] inline constexpr std::uint32_t
encode(unsigned size_code, unsigned operation, unsigned rm, unsigned option,
       bool scaled, unsigned mode, unsigned rn, unsigned rt) {
  return 0x3820'0000U | (size_code << 30) | (operation << 22) |
         ((rm & 31U) << 16) | ((option & 7U) << 13) |
         (static_cast<unsigned>(scaled) << 12) | ((mode & 3U) << 10) |
         ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline arm64::CpuState seeded_state(std::uint64_t seed) {
  arm64::CpuState cpu{};
  for (unsigned index = 0; index < cpu.registers.size(); ++index)
    cpu.registers[index] =
        seed ^ (0x8000'0000'1000'0000ULL |
                (static_cast<std::uint64_t>(index) << 8) | index);
  cpu.sp = seed ^ 0x1234'5678'9abc'def0ULL;
  cpu.nzcv = 0xa000'0000U;
  cpu.fpcr = 0x0040'0000U;
  cpu.fpsr = 0x0800'009fU;
  cpu.tls = seed ^ 0xfedc'ba98'7654'3210ULL;
  for (unsigned index = 0; index < cpu.vectors.size(); ++index)
    for (unsigned byte = 0; byte < cpu.vectors[index].bytes.size(); ++byte)
      cpu.vectors[index].bytes[byte] =
          static_cast<std::byte>(seed + index * 23U + byte);
  return cpu;
}

[[nodiscard]] inline constexpr std::uint64_t sign_extend(std::uint64_t value,
                                                         unsigned bits) {
  const auto mask =
      bits == 64U ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1U;
  const auto sign = std::uint64_t{1} << (bits - 1U);
  value &= mask;
  return (value ^ sign) - sign;
}

[[nodiscard]] inline constexpr std::uint64_t
extend_offset(std::uint64_t value, unsigned option, unsigned shift) {
  switch (option) {
  case 2:
    value &= 0xffff'ffffU;
    break;
  case 3:
    break;
  case 6:
    value = sign_extend(value & 0xffff'ffffU, 32);
    break;
  case 7:
    break;
  default:
    return 0;
  }
  return value << shift;
}

[[nodiscard]] inline constexpr std::uint64_t
read_register(const arm64::CpuState &cpu, unsigned index, bool stack_pointer) {
  if (index != 31U)
    return cpu.registers[index];
  return stack_pointer ? cpu.sp : 0;
}

[[nodiscard]] inline constexpr std::uint64_t
address(const arm64::CpuState &cpu, const IntegerVariant &variant,
        unsigned option, bool scaled, unsigned rm, unsigned rn) {
  return read_register(cpu, rn, true) +
         extend_offset(read_register(cpu, rm, false), option,
                       scaled ? variant.size_code : 0U);
}

[[nodiscard]] inline std::uint64_t matrix_raw(unsigned option,
                                              unsigned register_index) {
  const auto magnitude = 1U + (register_index & 15U);
  switch (option) {
  case 2:
    return 0xfedc'ba98'0000'0000ULL | magnitude;
  case 3:
    return magnitude;
  case 6:
    return 0x1234'5678'0000'0000ULL |
           static_cast<std::uint32_t>(0U - magnitude);
  case 7:
    return 0U - static_cast<std::uint64_t>(magnitude);
  default:
    return 0;
  }
}

[[nodiscard]] inline arm64::CpuState
make_initial(const IntegerVariant &variant, unsigned variant_index,
             unsigned option, bool scaled, Registers registers,
             std::uint64_t raw, std::uint64_t desired_address,
             std::uint64_t seed) {
  auto initial = seeded_state(seed);
  /*
   * Assign the index first and transfer second. When Rm==Rt this models the
   * one architectural register shared by both operands.
   */
  if (registers.rm != 31U)
    initial.registers[registers.rm] = raw;
  if (registers.rt != 31U)
    initial.registers[registers.rt] =
        transfer_value + variant_index * 0x101U;

  const auto offset =
      extend_offset(read_register(initial, registers.rm, false), option,
                    scaled ? variant.size_code : 0U);
  if (registers.rn == 31U) {
    /* Ordinary success cases must never accidentally exercise a malformed
     * SP. Dedicated fault tests cover that state and its ordering.
     */
    initial.sp = (desired_address - offset) & ~std::uint64_t{15};
  } else if (registers.rn != registers.rm && registers.rn != registers.rt) {
    initial.registers[registers.rn] = desired_address - offset;
  }
  return initial;
}

[[nodiscard]] inline constexpr std::uint64_t
expected_load(const IntegerVariant &variant, std::uint64_t raw) {
  if (variant.operation < 2U)
    return variant.size == 8U
               ? raw
               : raw & ((std::uint64_t{1} << (variant.size * 8U)) - 1U);
  auto value = sign_extend(raw, static_cast<unsigned>(variant.size * 8U));
  if (variant.operation == 3U)
    value &= 0xffff'ffffU;
  return value;
}

inline void fill_memory(std::span<std::byte> image, unsigned salt) {
  for (std::size_t offset = 0; offset < image.size(); ++offset)
    image[offset] =
        static_cast<std::byte>(0x80U | ((offset * 37U + salt * 29U) & 0x7fU));
}

[[nodiscard]] inline std::uint64_t
decode_little_endian(std::span<const std::byte> image, std::size_t offset,
                     std::size_t size) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < size; ++index)
    value |= static_cast<std::uint64_t>(
                 std::to_integer<unsigned>(image[offset + index]))
             << (index * 8U);
  return value;
}

inline void encode_little_endian(std::span<std::byte> image, std::size_t offset,
                                 std::size_t size, std::uint64_t value) {
  for (std::size_t index = 0; index < size; ++index)
    image[offset + index] = static_cast<std::byte>(value >> (index * 8U));
}

[[nodiscard]] inline constexpr std::uint64_t page_start(std::uint64_t value) {
  return value & ~(static_cast<std::uint64_t>(page_size) - 1U);
}

inline void require(bool condition, std::string_view message) {
  if (condition)
    return;
  std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()),
               message.data());
  std::abort();
}

inline void require_engine_counts(const engine_tests::EngineHarness &harness,
                                  std::size_t expected) {
  const auto &counts = harness.counts();
  require(counts.interpreter == expected,
          "interpreter did not run the exact expected register-memory cases");
  require(counts.gadgets == expected,
          "gadgets did not run the exact expected register-memory cases");
  require(counts.jit == expected,
          "JIT did not run the exact expected baseline register-memory cases");
  require(counts.jit_skipped == 0U,
          "baseline register-memory case was unexpectedly skipped by JIT");
  require(counts.jit_skipped_host_feature == 0U,
          "baseline register-memory case was feature-skipped by JIT");
  require(counts.jit_unavailable == 0U,
          "JIT engine is unavailable in this build");
}

inline void check_transfer(
    engine_tests::EngineHarness &harness, std::string_view name,
    const IntegerVariant &variant, unsigned option, bool scaled,
    Registers registers, const arm64::CpuState &initial, unsigned salt,
    std::optional<std::uint64_t> initial_value = std::nullopt) {
  if (registers.rn == 31U)
    require((initial.sp & 15U) == 0U,
            "success oracle received a misaligned SP");
  const auto transfer_address =
      address(initial, variant, option, scaled, registers.rm, registers.rn);
  const auto region_address = page_start(transfer_address);
  std::vector<std::byte> initial_memory(2U * page_size);
  fill_memory(initial_memory, salt);
  const auto memory_offset =
      static_cast<std::size_t>(transfer_address - region_address);
  require(memory_offset + variant.size <= initial_memory.size(),
          "register-memory oracle generated an out-of-range address");
  if (initial_value)
    encode_little_endian(initial_memory, memory_offset, variant.size,
                         *initial_value);

  auto expected = initial;
  auto expected_memory = initial_memory;
  if (variant.load) {
    const auto raw =
        decode_little_endian(initial_memory, memory_offset, variant.size);
    if (registers.rt != 31U)
      expected.registers[registers.rt] = expected_load(variant, raw);
  } else {
    encode_little_endian(expected_memory, memory_offset, variant.size,
                         read_register(initial, registers.rt, false));
  }
  const std::array regions{
      engine_tests::MemoryRegion{
          .address = region_address,
          .initial = initial_memory,
          .expected = expected_memory,
      },
  };
  harness.check({
      .name = name,
      .instruction =
          encode(variant.size_code, variant.operation, registers.rm, option,
                 scaled, 2, registers.rn, registers.rt),
      .initial = initial,
      .expected = expected,
      .native = engine_tests::NativeRequirements::baseline(),
      .memory = std::span<const engine_tests::MemoryRegion>{regions},
  });
}

} // namespace arm64_load_store_register_test
