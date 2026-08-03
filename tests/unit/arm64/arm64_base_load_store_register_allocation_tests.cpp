// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_load_store_register_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_load_store_register_test;
namespace engine_tests = uml::emulator::tests;

namespace {

void invalid_index_extensions(engine_tests::EngineHarness &harness) {
  auto initial = test::seeded_state(0x8100'0000'0000'0000ULL);
  initial.sp = test::data_base + 1U;
  initial.registers[3] = test::transfer_value;
  /* S has no independently reserved value. Pair every reserved option with
   * both S values so all invalid option/S combinations execute in every
   * engine.
   */
  for (const auto &variant : test::integer_variants)
    for (const auto option : test::illegal_options)
      for (const bool scaled : {false, true})
        for (unsigned rm = 0; rm < 32; ++rm) {
          char name[112];
          std::snprintf(
              name, sizeof(name), "%.*s reserved option=%u scaled=%u rm=%u",
              static_cast<int>(variant.name.size()), variant.name.data(),
              option, static_cast<unsigned>(scaled), rm);
          harness.check_illegal(name,
                                test::encode(variant.size_code,
                                             variant.operation, rm, option,
                                             scaled, 2, 31, 3),
                                initial);
        }
}

void invalid_transfer_widths(engine_tests::EngineHarness &harness) {
  auto initial = test::seeded_state(0x8200'0000'0000'0000ULL);
  initial.sp = test::data_base + 1U;
  initial.registers[3] = test::transfer_value;
  for (const auto fields : {std::array{2U, 3U}, std::array{3U, 3U}})
    for (const auto option : test::legal_options)
      for (const bool scaled : {false, true})
        for (unsigned rm = 0; rm < 32; ++rm) {
          char name[112];
          std::snprintf(name, sizeof(name),
                        "reserved size=%u operation=%u option=%u "
                        "scaled=%u rm=%u",
                        fields[0], fields[1], option,
                        static_cast<unsigned>(scaled), rm);
          harness.check_illegal(
              name,
              test::encode(fields[0], fields[1], rm, option, scaled, 2, 31, 3),
              initial);
        }
}

void non_register_addressing_patterns(engine_tests::EngineHarness &harness) {
  auto initial = test::seeded_state(0x8300'0000'0000'0000ULL);
  initial.registers[2] = test::data_base;
  initial.registers[3] = test::transfer_value;
  for (const auto &variant : test::integer_variants)
    for (const unsigned mode : {1U, 3U})
      for (unsigned rm = 0; rm < 32; ++rm)
        for (unsigned option = 0; option < 8; ++option)
          for (const bool scaled : {false, true}) {
            const auto instruction =
                test::encode(variant.size_code, variant.operation, rm, option,
                             scaled, mode, 2, 3);
            /* This bit range belongs to the authenticated
             * immediate-load family rather than this family. */
            if ((instruction & 0xff20'0400U) == 0xf820'0400U)
              continue;
            char name[120];
            std::snprintf(name, sizeof(name),
                          "reserved mode=%u size=%u operation=%u "
                          "option=%u scaled=%u rm=%u",
                          mode, variant.size_code, variant.operation, option,
                          static_cast<unsigned>(scaled), rm);
            harness.check_illegal(name, instruction, initial);
          }
}

void invalid_prefetch_extensions(engine_tests::EngineHarness &harness) {
  auto initial = test::seeded_state(0x8500'0000'0000'0000ULL);
  initial.sp = test::data_base + 1U;
  for (const auto option : test::illegal_options)
    for (const bool scaled : {false, true})
      for (unsigned rm = 0; rm < 32; ++rm) {
        char name[104];
        std::snprintf(name, sizeof(name),
                      "reserved prfm option=%u scaled=%u rm=%u", option,
                      static_cast<unsigned>(scaled), rm);
        harness.check_illegal(
            name, test::encode(3, 2, rm, option, scaled, 2, 31, 7), initial);
      }
}

void invalid_prefetch_register_fields(
    engine_tests::EngineHarness &harness) {
  auto initial = test::seeded_state(0x8700'0000'0000'0000ULL);
  initial.sp = test::data_base + 1U;
  for (unsigned rn = 0; rn < 32; ++rn)
    for (unsigned operation = 0; operation < 32; ++operation) {
      char name[104];
      std::snprintf(name, sizeof(name),
                    "reserved prfm fields rn=%u operation=%u", rn,
                    operation);
      harness.check_illegal(
          name, test::encode(3, 2, 4, 0, false, 2, rn, operation), initial);
    }
}

void register_fields_do_not_change_allocation(
    engine_tests::EngineHarness &harness) {
  auto initial = test::seeded_state(0x8400'0000'0000'0000ULL);
  initial.sp = test::data_base + 1U;
  for (unsigned rn = 0; rn < 32; ++rn)
    for (unsigned rt = 0; rt < 32; ++rt) {
      char name[96];
      std::snprintf(name, sizeof(name),
                    "reserved option field sweep rn=%u rt=%u", rn, rt);
      harness.check_illegal(name, test::encode(0, 1, 4, 0, false, 2, rn, rt),
                            initial);
    }
}

} // namespace

int main() {
  engine_tests::EngineHarness harness;
  invalid_index_extensions(harness);
  invalid_transfer_widths(harness);
  non_register_addressing_patterns(harness);
  register_fields_do_not_change_allocation(harness);
  invalid_prefetch_extensions(harness);
  invalid_prefetch_register_fields(harness);
  test::require_engine_counts(harness, 17408U);
  harness.print_summary("arm64-base-load-store-register-allocation");
  return 0;
}
