// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_load_store_register_test_support.hpp"

#include <array>
#include <cstdio>
#include <optional>
#include <span>
#include <vector>

namespace test = arm64_load_store_register_test;
namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

namespace {

using test::Registers;
using test::check_transfer;
using test::make_initial;
using test::matrix_raw;

void operation_extension_shift_and_register_fields(
    engine_tests::EngineHarness &harness) {
  for (unsigned variant_index = 0;
       variant_index < test::integer_variants.size(); ++variant_index) {
    const auto &variant = test::integer_variants[variant_index];
    for (const auto option : test::legal_options)
      for (const bool scaled : {false, true}) {
        for (unsigned rm = 0; rm < 32; ++rm) {
          const Registers registers{rm, 26, 27};
          const auto raw = registers.rn == registers.rm
                               ? 0x0010'0000U
                               : matrix_raw(option, rm);
          auto initial = make_initial(
              variant, variant_index, option, scaled, registers, raw,
              test::data_base + 0x400U + rm * 16U,
              0x1000'0000'0000'0000ULL |
                  (static_cast<std::uint64_t>(variant_index) << 32) |
                  (option << 16) | (static_cast<unsigned>(scaled) << 8) | rm);
          char name[112];
          std::snprintf(name, sizeof(name), "%.*s option=%u scaled=%u rm=%u",
                        static_cast<int>(variant.name.size()),
                        variant.name.data(), option,
                        static_cast<unsigned>(scaled), rm);
          check_transfer(harness, name, variant, option, scaled, registers,
                         initial, variant_index * 17U + rm);
        }

        for (unsigned rn = 0; rn < 32; ++rn) {
          const Registers registers{24, rn, 25};
          const auto raw = registers.rn == registers.rm
                               ? 0x0011'0000U
                               : matrix_raw(option, rn);
          auto initial = make_initial(
              variant, variant_index, option, scaled, registers, raw,
              test::data_base + 0x800U + rn * 16U,
              0x2000'0000'0000'0000ULL |
                  (static_cast<std::uint64_t>(variant_index) << 32) |
                  (option << 16) | (static_cast<unsigned>(scaled) << 8) | rn);
          char name[112];
          std::snprintf(name, sizeof(name), "%.*s option=%u scaled=%u rn=%u",
                        static_cast<int>(variant.name.size()),
                        variant.name.data(), option,
                        static_cast<unsigned>(scaled), rn);
          check_transfer(harness, name, variant, option, scaled, registers,
                         initial, variant_index * 19U + rn);
        }

        for (unsigned rt = 0; rt < 32; ++rt) {
          const Registers registers{22, 23, rt};
          auto initial = make_initial(
              variant, variant_index, option, scaled, registers,
              matrix_raw(option, rt), test::data_base + 0xc00U + rt * 16U,
              0x3000'0000'0000'0000ULL |
                  (static_cast<std::uint64_t>(variant_index) << 32) |
                  (option << 16) | (static_cast<unsigned>(scaled) << 8) | rt);
          char name[112];
          std::snprintf(name, sizeof(name), "%.*s option=%u scaled=%u rt=%u",
                        static_cast<int>(variant.name.size()),
                        variant.name.data(), option,
                        static_cast<unsigned>(scaled), rt);
          check_transfer(harness, name, variant, option, scaled, registers,
                         initial, variant_index * 23U + rt);
        }
      }
  }
}

void extension_boundaries(engine_tests::EngineHarness &harness) {
  constexpr std::array raw_values{
      std::uint64_t{0},
      std::uint64_t{1},
      std::uint64_t{0x7fff'ffffU},
      std::uint64_t{0x8000'0000U},
      std::uint64_t{0xffff'ffffU},
      std::uint64_t{0x7fff'ffff'ffff'ffffULL},
      std::uint64_t{0x8000'0000'0000'0000ULL},
      std::uint64_t{0xffff'ffff'ffff'ffffULL},
  };
  for (unsigned variant_index = 0;
       variant_index < test::integer_variants.size(); ++variant_index) {
    const auto &variant = test::integer_variants[variant_index];
    for (const auto option : test::legal_options)
      for (const bool scaled : {false, true})
        for (unsigned raw_index = 0; raw_index < raw_values.size();
             ++raw_index) {
          const Registers registers{4, 2, 3};
          auto initial = make_initial(
              variant, variant_index, option, scaled, registers,
              raw_values[raw_index],
              test::data_base + 0x1000U + raw_index * 32U,
              0x4000'0000'0000'0000ULL |
                  (static_cast<std::uint64_t>(variant_index) << 32) |
                  (option << 16) | (static_cast<unsigned>(scaled) << 8) |
                  raw_index);
          char name[120];
          std::snprintf(name, sizeof(name), "%.*s option=%u scaled=%u raw=%u",
                        static_cast<int>(variant.name.size()),
                        variant.name.data(), option,
                        static_cast<unsigned>(scaled), raw_index);
          check_transfer(harness, name, variant, option, scaled, registers,
                         initial, variant_index * 31U + raw_index);
        }
  }
}

void transfer_value_boundaries(engine_tests::EngineHarness &harness) {
  constexpr std::array source_values{
      std::uint64_t{0},
      std::uint64_t{0x0123'4567'89ab'cdefULL},
      std::uint64_t{0x8000'0000'0000'0000ULL},
      std::uint64_t{0xffff'ffff'ffff'ffffULL},
  };
  for (unsigned variant_index = 0;
       variant_index < test::integer_variants.size(); ++variant_index) {
    const auto &variant = test::integer_variants[variant_index];
    const auto sign_bit = std::uint64_t{1}
                          << (static_cast<unsigned>(variant.size * 8U) - 1U);
    const std::array memory_values{
        std::uint64_t{0},
        sign_bit - 1U,
        sign_bit,
        variant.size == 8U ? ~std::uint64_t{0}
                           : (std::uint64_t{1} << (variant.size * 8U)) - 1U,
    };
    for (const auto option : test::legal_options)
      for (const bool scaled : {false, true})
        for (unsigned value_index = 0; value_index < source_values.size();
             ++value_index) {
          const Registers registers{4, 2, 3};
          auto initial = make_initial(
              variant, variant_index, option, scaled, registers,
              matrix_raw(option, value_index),
              test::data_base + 0x1200U + value_index * 32U,
              0x4800'0000'0000'0000ULL |
                  (static_cast<std::uint64_t>(variant_index) << 32) |
                  (option << 16) | (static_cast<unsigned>(scaled) << 8) |
                  value_index);
          std::optional<std::uint64_t> initial_value;
          if (variant.load) {
            initial_value = memory_values[value_index];
          } else {
            initial.registers[registers.rt] = source_values[value_index];
          }
          char name[120];
          std::snprintf(
              name, sizeof(name), "%.*s option=%u scaled=%u transfer-value=%u",
              static_cast<int>(variant.name.size()), variant.name.data(),
              option, static_cast<unsigned>(scaled), value_index);
          check_transfer(harness, name, variant, option, scaled, registers,
                         initial, variant_index * 33U + value_index,
                         initial_value);
        }
  }
}

void explicit_aliases(engine_tests::EngineHarness &harness) {
  constexpr std::array alias_sets{
      Registers{6, 6, 7},  Registers{6, 7, 6},    Registers{6, 7, 7},
      Registers{6, 6, 6},  Registers{31, 6, 7},   Registers{6, 31, 7},
      Registers{6, 7, 31}, Registers{31, 31, 31},
  };
  for (unsigned variant_index = 0;
       variant_index < test::integer_variants.size(); ++variant_index) {
    const auto &variant = test::integer_variants[variant_index];
    for (const auto option : test::legal_options)
      for (const bool scaled : {false, true})
        for (unsigned alias_index = 0; alias_index < alias_sets.size();
             ++alias_index) {
          const auto registers = alias_sets[alias_index];
          const auto raw = registers.rn == registers.rm
                               ? 0x0012'0000U
                               : matrix_raw(option, alias_index);
          auto initial = make_initial(
              variant, variant_index, option, scaled, registers, raw,
              test::data_base + 0x1400U + alias_index * 32U,
              0x5000'0000'0000'0000ULL |
                  (static_cast<std::uint64_t>(variant_index) << 32) |
                  (option << 16) | (static_cast<unsigned>(scaled) << 8) |
                  alias_index);
          char name[120];
          std::snprintf(name, sizeof(name), "%.*s option=%u scaled=%u alias=%u",
                        static_cast<int>(variant.name.size()),
                        variant.name.data(), option,
                        static_cast<unsigned>(scaled), alias_index);
          check_transfer(harness, name, variant, option, scaled, registers,
                         initial, variant_index * 37U + alias_index);
        }
  }
}

void aligned_sp_effective_addresses(engine_tests::EngineHarness &harness) {
  for (unsigned variant_index = 0;
       variant_index < test::integer_variants.size(); ++variant_index) {
    const auto &variant = test::integer_variants[variant_index];
    for (const auto option : test::legal_options)
      for (const bool scaled : {false, true})
        for (unsigned residue = 0;
             residue < (scaled ? 1U : variant.size); ++residue) {
          const Registers registers{4, 31, 3};
          auto initial = test::seeded_state(
              0x5800'0000'0000'0000ULL |
              (static_cast<std::uint64_t>(variant_index) << 32) |
              (option << 16) | (static_cast<unsigned>(scaled) << 8) |
              residue);
          initial.sp = test::data_base + 0x1c00U;
          initial.registers[registers.rm] = residue;
          initial.registers[registers.rt] =
              test::transfer_value + variant_index * 0x101U;
          char name[120];
          std::snprintf(
              name, sizeof(name),
              "%.*s option=%u scaled=%u aligned-SP effective-residue=%u",
              static_cast<int>(variant.name.size()), variant.name.data(),
              option, static_cast<unsigned>(scaled), residue);
          check_transfer(harness, name, variant, option, scaled, registers,
                         initial, variant_index * 39U + residue);
        }
  }
}

void unaligned_and_cross_page(engine_tests::EngineHarness &harness) {
  for (unsigned variant_index = 0;
       variant_index < test::integer_variants.size(); ++variant_index) {
    const auto &variant = test::integer_variants[variant_index];
    for (const auto option : test::legal_options)
      for (const bool scaled : {false, true}) {
        for (unsigned residue = 0; residue < variant.size; ++residue) {
          const Registers registers{4, 2, 3};
          auto initial = make_initial(
              variant, variant_index, option, scaled, registers,
              matrix_raw(option, residue), test::data_base + 0x1800U + residue,
              0x6000'0000'0000'0000ULL |
                  (static_cast<std::uint64_t>(variant_index) << 32) |
                  (option << 16) | (static_cast<unsigned>(scaled) << 8) |
                  residue);
          char name[120];
          std::snprintf(
              name, sizeof(name), "%.*s option=%u scaled=%u residue=%u",
              static_cast<int>(variant.name.size()), variant.name.data(),
              option, static_cast<unsigned>(scaled), residue);
          check_transfer(harness, name, variant, option, scaled, registers,
                         initial, variant_index * 41U + residue);
        }
        if (variant.size == 1U)
          continue;
        const Registers registers{4, 2, 3};
        const auto target =
            test::data_base + test::page_size - variant.size / 2U;
        auto initial = make_initial(
            variant, variant_index, option, scaled, registers,
            matrix_raw(option, variant_index), target,
            0x7000'0000'0000'0000ULL |
                (static_cast<std::uint64_t>(variant_index) << 32) |
                (option << 16) | (static_cast<unsigned>(scaled) << 8));
        char name[112];
        std::snprintf(name, sizeof(name), "%.*s option=%u scaled=%u cross-page",
                      static_cast<int>(variant.name.size()),
                      variant.name.data(), option,
                      static_cast<unsigned>(scaled));
        check_transfer(harness, name, variant, option, scaled, registers,
                       initial, variant_index * 43U);
      }
  }
}

} // namespace

int main() {
  engine_tests::EngineHarness harness;
  operation_extension_shift_and_register_fields(harness);
  extension_boundaries(harness);
  transfer_value_boundaries(harness);
  explicit_aliases(harness);
  aligned_sp_effective_addresses(harness);
  unaligned_and_cross_page(harness);
  test::require_engine_counts(harness, 12668U);
  harness.print_summary("arm64-base-load-store-register");
  return 0;
}
