// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_load_store_register_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_load_store_register_test;
namespace engine_tests = uml::emulator::tests;

namespace {

using test::Registers;

void check_prefetch(engine_tests::EngineHarness &harness,
                    std::string_view name, unsigned rm, unsigned option,
                    bool scaled, unsigned rn, unsigned operation,
                    const uml::cpu::arm64::CpuState &initial) {
  harness.check_one(
      name, test::encode(3, 2, rm, option, scaled, 2, rn, operation), initial,
      initial, engine_tests::NativeRequirements::baseline());
}

void every_field_value(engine_tests::EngineHarness &harness) {
  for (const auto option : test::legal_options)
    for (const bool scaled : {false, true}) {
      for (unsigned rm = 0; rm < 32; ++rm) {
        auto initial = test::seeded_state(
            0x9000'0000'0000'0000ULL | (option << 16) |
            (static_cast<std::uint64_t>(scaled) << 8) | rm);
        initial.registers[26] = test::data_base + 32U * test::page_size;
        if (rm != 31U)
          initial.registers[rm] = test::matrix_raw(option, rm);
        char name[96];
        std::snprintf(name, sizeof(name),
                      "prfm option=%u scaled=%u Rm=%u", option,
                      static_cast<unsigned>(scaled), rm);
        check_prefetch(harness, name, rm, option, scaled, 26, 27, initial);
      }

      for (unsigned rn = 0; rn < 32; ++rn) {
        auto initial = test::seeded_state(
            0x9100'0000'0000'0000ULL | (option << 16) |
            (static_cast<std::uint64_t>(scaled) << 8) | rn);
        initial.registers[24] = test::matrix_raw(option, rn);
        if (rn == 31U)
          initial.sp = test::data_base + 32U * test::page_size;
        else if (rn != 24U)
          initial.registers[rn] =
              test::data_base + 32U * test::page_size;
        char name[96];
        std::snprintf(name, sizeof(name),
                      "prfm option=%u scaled=%u Rn=%u", option,
                      static_cast<unsigned>(scaled), rn);
        check_prefetch(harness, name, 24, option, scaled, rn, 25, initial);
      }

      for (unsigned operation = 0; operation < 32; ++operation) {
        auto initial = test::seeded_state(
            0x9200'0000'0000'0000ULL | (option << 16) |
            (static_cast<std::uint64_t>(scaled) << 8) | operation);
        initial.registers[22] =
            test::data_base + 32U * test::page_size;
        initial.registers[23] = test::matrix_raw(option, operation);
        char name[104];
        std::snprintf(name, sizeof(name),
                      "prfm option=%u scaled=%u operation=%u", option,
                      static_cast<unsigned>(scaled), operation);
        check_prefetch(harness, name, 23, option, scaled, 22, operation,
                       initial);
      }
    }
}

void every_alias_shape(engine_tests::EngineHarness &harness) {
  constexpr std::array aliases{
      Registers{6, 6, 7},  Registers{6, 7, 6},    Registers{6, 7, 7},
      Registers{6, 6, 6},  Registers{31, 6, 7},   Registers{6, 31, 7},
      Registers{6, 7, 31}, Registers{31, 31, 31},
  };
  for (const auto option : test::legal_options)
    for (const bool scaled : {false, true})
      for (unsigned alias_index = 0; alias_index < aliases.size();
           ++alias_index) {
        const auto registers = aliases[alias_index];
        auto initial = test::seeded_state(
            0x9300'0000'0000'0000ULL | (option << 16) |
            (static_cast<std::uint64_t>(scaled) << 8) | alias_index);
        if (registers.rm != 31U)
          initial.registers[registers.rm] =
              test::matrix_raw(option, alias_index);
        if (registers.rn == 31U)
          initial.sp = test::data_base + 32U * test::page_size;
        char name[104];
        std::snprintf(name, sizeof(name),
                      "prfm option=%u scaled=%u alias=%u", option,
                      static_cast<unsigned>(scaled), alias_index);
        check_prefetch(harness, name, registers.rm, option, scaled,
                       registers.rn, registers.rt, initial);
      }
}

void misaligned_sp_and_unmapped_address_remain_hint(
    engine_tests::EngineHarness &harness) {
  for (const auto option : test::legal_options)
    for (const bool scaled : {false, true})
      for (unsigned rm = 0; rm < 32; ++rm) {
        auto initial = test::seeded_state(
            0x9400'0000'0000'0000ULL | (option << 16) |
            (static_cast<std::uint64_t>(scaled) << 8) | rm);
        initial.sp = test::data_base + 32U * test::page_size + 1U;
        if (rm != 31U)
          initial.registers[rm] = test::matrix_raw(option, rm);
        char name[112];
        std::snprintf(name, sizeof(name),
                      "prfm option=%u scaled=%u misaligned-SP Rm=%u", option,
                      static_cast<unsigned>(scaled), rm);
        check_prefetch(harness, name, rm, option, scaled, 31, rm, initial);
      }
}

} // namespace

int main() {
  engine_tests::EngineHarness harness;
  every_field_value(harness);
  every_alias_shape(harness);
  misaligned_sp_and_unmapped_address_remain_hint(harness);
  test::require_engine_counts(harness, 1088U);
  harness.print_summary("arm64-base-load-store-register-prefetch");
  return 0;
}
