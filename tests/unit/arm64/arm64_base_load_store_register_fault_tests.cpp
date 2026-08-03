// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_load_store_register_test_support.hpp"

#include <array>
#include <cstdio>
#include <span>
#include <vector>

namespace test = arm64_load_store_register_test;
namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

namespace {

using test::Registers;

void permission_fault_atomicity(engine_tests::EngineHarness &harness) {
  for (unsigned variant_index = 0;
       variant_index < test::integer_variants.size(); ++variant_index) {
    const auto &variant = test::integer_variants[variant_index];
    for (const auto option : test::legal_options)
      for (const bool scaled : {false, true}) {
        std::vector<std::byte> memory(test::page_size);
        test::fill_memory(memory, variant_index * 53U + option);
        const auto fault_address =
            test::data_base + 0x200U + variant_index * 16U;
        const Registers registers{4, 2, variant.load ? 31U : 3U};
        auto initial = test::make_initial(
            variant, variant_index, option, scaled, registers,
            test::matrix_raw(option, variant_index), fault_address,
            0x7a00'0000'0000'0000ULL |
                (static_cast<std::uint64_t>(variant_index) << 32) |
                (option << 16) | static_cast<unsigned>(scaled));
        const std::array regions{
            engine_tests::MemoryRegion{
                .address = test::data_base,
                .initial = memory,
                .expected = memory,
                .readable = !variant.load,
                .writable = variant.load,
            },
        };
        char name[120];
        std::snprintf(
            name, sizeof(name), "%.*s option=%u scaled=%u permission fault",
            static_cast<int>(variant.name.size()), variant.name.data(), option,
            static_cast<unsigned>(scaled));
        harness.check({
            .name = name,
            .instruction =
                test::encode(variant.size_code, variant.operation, registers.rm,
                             option, scaled, 2, registers.rn, registers.rt),
            .initial = initial,
            .expected = initial,
            .event = engine_tests::ExpectedEvent::memory_fault,
            .native = engine_tests::NativeRequirements::baseline(),
            .fault =
                {
                    .address = fault_address,
                    .access = variant.load ? arm64::MemoryAccess::read
                                           : arm64::MemoryAccess::write,
                },
            .compare_fault = true,
            .memory = std::span<const engine_tests::MemoryRegion>{regions},
        });
      }
  }
}

void unmapped_fault_atomicity(engine_tests::EngineHarness &harness) {
  for (unsigned variant_index = 0;
       variant_index < test::integer_variants.size(); ++variant_index) {
    const auto &variant = test::integer_variants[variant_index];
    for (const auto option : test::legal_options)
      for (const bool scaled : {false, true}) {
        const Registers registers{4, 2, 3};
        const auto fault_address = test::data_base + 8U * test::page_size +
                                   variant_index * 256U + option * 16U +
                                   static_cast<unsigned>(scaled) * 8U;
        auto initial = test::make_initial(
            variant, variant_index, option, scaled, registers,
            test::matrix_raw(option, variant_index), fault_address,
            0x7800'0000'0000'0000ULL |
                (static_cast<std::uint64_t>(variant_index) << 32) |
                (option << 16) | static_cast<unsigned>(scaled));
        char name[112];
        std::snprintf(
            name, sizeof(name), "%.*s option=%u scaled=%u unmapped fault",
            static_cast<int>(variant.name.size()), variant.name.data(), option,
            static_cast<unsigned>(scaled));
        harness.check({
            .name = name,
            .instruction =
                test::encode(variant.size_code, variant.operation, registers.rm,
                             option, scaled, 2, registers.rn, registers.rt),
            .initial = initial,
            .expected = initial,
            .event = engine_tests::ExpectedEvent::memory_fault,
            .native = engine_tests::NativeRequirements::baseline(),
            .fault =
                {
                    .address = fault_address,
                    .access = variant.load ? arm64::MemoryAccess::read
                                           : arm64::MemoryAccess::write,
                },
            .compare_fault = true,
        });
      }
  }
}

void cross_page_fault_atomicity(engine_tests::EngineHarness &harness) {
  for (unsigned variant_index = 0;
       variant_index < test::integer_variants.size(); ++variant_index) {
    const auto &variant = test::integer_variants[variant_index];
    if (variant.size == 1U)
      continue;
    for (const auto option : test::legal_options)
      for (const bool scaled : {false, true}) {
        std::vector<std::byte> memory(test::page_size);
        test::fill_memory(memory, variant_index * 47U + option);
        const auto fault_address =
            test::data_base + test::page_size - variant.size / 2U;
        const Registers registers{4, 2, 3};
        auto initial = test::make_initial(
            variant, variant_index, option, scaled, registers,
            test::matrix_raw(option, variant_index), fault_address,
            0x7900'0000'0000'0000ULL |
                (static_cast<std::uint64_t>(variant_index) << 32) |
                (option << 16) | static_cast<unsigned>(scaled));
        const std::array regions{
            engine_tests::MemoryRegion{
                .address = test::data_base,
                .initial = memory,
                .expected = memory,
            },
        };
        char name[120];
        std::snprintf(
            name, sizeof(name), "%.*s option=%u scaled=%u cross-page fault",
            static_cast<int>(variant.name.size()), variant.name.data(), option,
            static_cast<unsigned>(scaled));
        harness.check({
            .name = name,
            .instruction =
                test::encode(variant.size_code, variant.operation, registers.rm,
                             option, scaled, 2, registers.rn, registers.rt),
            .initial = initial,
            .expected = initial,
            .event = engine_tests::ExpectedEvent::memory_fault,
            .native = engine_tests::NativeRequirements::baseline(),
            .fault =
                {
                    .address = test::data_base + test::page_size,
                    .access = variant.load ? arm64::MemoryAccess::read
                                           : arm64::MemoryAccess::write,
                },
            .compare_fault = true,
            .memory = std::span<const engine_tests::MemoryRegion>{regions},
        });
      }
  }
}

void misaligned_sp_precedes_effective_address(
    engine_tests::EngineHarness &harness) {
  for (unsigned variant_index = 0;
       variant_index < test::integer_variants.size(); ++variant_index) {
    const auto &variant = test::integer_variants[variant_index];
    for (const auto option : test::legal_options)
      for (const bool scaled : {false, true}) {
        std::vector<std::byte> memory(test::page_size);
        test::fill_memory(memory, variant_index * 59U + option);
        auto initial = test::seeded_state(
            0x7b00'0000'0000'0000ULL |
            (static_cast<std::uint64_t>(variant_index) << 32) |
            (option << 16) | static_cast<unsigned>(scaled));
        const unsigned shift = scaled ? variant.size_code : 0U;
        const auto effective_address = test::data_base + 0x310U;
        initial.sp = effective_address - (std::uint64_t{1} << shift);
        initial.registers[4] = 1U;
        initial.registers[3] = test::transfer_value;
        test::require((initial.sp & 15U) != 0U,
                      "misaligned-SP fault case accidentally aligned SP");
        test::require(
            test::address(initial, variant, option, scaled, 4, 31) ==
                effective_address,
            "misaligned-SP fault case did not produce its aligned address");
        const std::array regions{
            engine_tests::MemoryRegion{
                .address = test::data_base,
                .initial = memory,
                .expected = memory,
            },
        };
        char name[128];
        std::snprintf(name, sizeof(name),
                      "%.*s option=%u scaled=%u SP alignment precedes address",
                      static_cast<int>(variant.name.size()),
                      variant.name.data(), option,
                      static_cast<unsigned>(scaled));
        harness.check({
            .name = name,
            .instruction =
                test::encode(variant.size_code, variant.operation, 4, option,
                             scaled, 2, 31, 3),
            .initial = initial,
            .expected = initial,
            .event = engine_tests::ExpectedEvent::memory_fault,
            .native = engine_tests::NativeRequirements::baseline(),
            .fault =
                {
                    .address = initial.sp,
                    .access = variant.load ? arm64::MemoryAccess::read
                                           : arm64::MemoryAccess::write,
                },
            .compare_fault = true,
            .memory = std::span<const engine_tests::MemoryRegion>{regions},
        });
      }
  }
}

} // namespace

int main() {
  engine_tests::EngineHarness harness;
  permission_fault_atomicity(harness);
  unmapped_fault_atomicity(harness);
  cross_page_fault_atomicity(harness);
  misaligned_sp_precedes_effective_address(harness);
  test::require_engine_counts(harness, 384U);
  harness.print_summary("arm64-base-load-store-register-fault");
  return 0;
}
