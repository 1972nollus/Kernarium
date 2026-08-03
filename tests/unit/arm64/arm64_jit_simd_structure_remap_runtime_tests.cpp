// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_structure_test_support.hpp"
#include "jit_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;
namespace structure = arm64_simd_structure_test;

namespace {

constexpr std::uint32_t system_call = 0xd400'0001U;
constexpr std::array modes{
    structure::AddressMode::no_offset,
    structure::AddressMode::post_immediate,
    structure::AddressMode::post_register,
};
constexpr std::array high_and_wrapping_vectors{0U, 15U, 16U, 31U};

[[nodiscard]] constexpr std::uint32_t
bitwise_xor(unsigned destination, unsigned left, unsigned right) noexcept {
  return 0x6e20'1c00U | ((right & 31U) << 16U) |
         ((left & 31U) << 5U) | (destination & 31U);
}

void apply_bitwise_xor(arm64::CpuState &cpu, unsigned destination,
                       unsigned left, unsigned right) {
  const auto left_value = cpu.vectors[left].bytes;
  const auto right_value = cpu.vectors[right].bytes;
  for (std::size_t byte = 0; byte < left_value.size(); ++byte)
    cpu.vectors[destination].bytes[byte] =
        left_value[byte] ^ right_value[byte];
}

void full_form_matrix(engine::EngineHarness &harness,
                      std::span<const std::byte> memory,
                      std::uint64_t &seed) {
  const auto &form = structure::multiple_forms.front();
  engine::require(form.opcode == 7U && form.register_count == 1U);
  const auto address =
      structure::data_base + structure::page_size / 2U + 3U;
  for (const bool full_vector : {false, true})
    for (const bool load : {false, true})
      for (unsigned size = 0; size < 4U; ++size)
        for (const auto mode : modes)
          for (const auto transfer : high_and_wrapping_vectors) {
            char name[176];
            std::snprintf(
                name, sizeof(name),
                "jit-structure-full-one q=%u load=%u size=%u mode=%u "
                "vt=%u",
                static_cast<unsigned>(full_vector),
                static_cast<unsigned>(load), size,
                static_cast<unsigned>(mode), transfer);
            structure::check_multiple_success(
                harness, name, form, full_vector, size, load, mode, 2U,
                transfer, 5U, address, memory, seed++);
          }
}

void lane_form_matrix(engine::EngineHarness &harness,
                      std::span<const std::byte> memory,
                      std::uint64_t &seed) {
  const auto address =
      structure::data_base + structure::page_size / 2U + 1U;
  for (const bool full_vector : {false, true})
    for (const bool load : {false, true})
      for (unsigned opcode = 0; opcode < 6U; ++opcode)
        for (unsigned scale_bit = 0; scale_bit < 2U; ++scale_bit)
          for (unsigned size = 0; size < 4U; ++size) {
            const auto form = structure::decode_single(
                full_vector, load, 0U, opcode, scale_bit, size);
            if (!form.valid || form.replicate ||
                form.register_count != 1U)
              continue;
            for (const auto mode : modes)
              for (const unsigned transfer : {0U, 31U}) {
                char name[208];
                std::snprintf(
                    name, sizeof(name),
                    "jit-structure-lane-one q=%u load=%u width=%zu "
                    "lane=%zu opcode=%u s=%u size=%u mode=%u vt=%u",
                    static_cast<unsigned>(full_vector),
                    static_cast<unsigned>(load), form.element_size,
                    form.lane, opcode, scale_bit, size,
                    static_cast<unsigned>(mode), transfer);
                structure::check_single_success(
                    harness, name, form, full_vector, load, 0U, opcode,
                    scale_bit, size, mode, 2U, transfer, 5U, address,
                    memory, seed++);
              }
          }
}

void replicate_form_matrix(engine::EngineHarness &harness,
                           std::span<const std::byte> memory,
                           std::uint64_t &seed) {
  const auto address =
      structure::data_base + structure::page_size / 2U + 1U;
  for (const bool full_vector : {false, true})
    for (unsigned size = 0; size < 4U; ++size) {
      const auto form = structure::decode_single(
          full_vector, true, 0U, 6U, 0U, size);
      engine::require(form.valid && form.replicate &&
                      form.register_count == 1U);
      for (const auto mode : modes)
        for (const unsigned transfer : {0U, 31U}) {
          char name[176];
          std::snprintf(
              name, sizeof(name),
              "jit-structure-replicate-one q=%u width=%zu mode=%u "
              "vt=%u",
              static_cast<unsigned>(full_vector), form.element_size,
              static_cast<unsigned>(mode), transfer);
          structure::check_single_success(
              harness, name, form, full_vector, true, 0U, 6U, 0U, size,
              mode, 2U, transfer, 5U, address, memory, seed++);
        }
    }
}

void check_full_dependency(engine::EngineHarness &harness,
                           std::string_view name, bool full_vector,
                           bool load, unsigned size,
                           structure::AddressMode mode,
                           std::span<const std::byte> memory,
                           std::uint64_t seed) {
  const auto &form = structure::multiple_forms.front();
  const auto address =
      structure::data_base + structure::page_size / 2U;
  auto initial = structure::seeded_state(seed);
  structure::set_base(initial, 2U, address);
  initial.registers[5] = 37U;
  auto expected = initial;
  auto expected_memory =
      std::vector<std::byte>(memory.begin(), memory.end());
  apply_bitwise_xor(expected, 31U, 3U, 4U);
  const auto before_memory = expected;
  structure::apply_multiple_oracle(
      before_memory, expected, memory, expected_memory,
      static_cast<std::size_t>(address - structure::data_base), form,
      full_vector, size, load, 31U);
  structure::apply_writeback(
      before_memory, expected, mode, 2U, 5U,
      structure::multiple_transfer_size(form, full_vector));
  apply_bitwise_xor(expected, 0U, 31U, 6U);
  expected.pc = engine::engine_test_code_base + 16U;

  const std::array code{
      engine::ProgramWord{0U, bitwise_xor(31U, 3U, 4U)},
      engine::ProgramWord{
          4U,
          structure::multiple_instruction(
              full_vector, load, structure::post_index(mode),
              structure::encoded_offset_register(mode, 5U), form.opcode,
              size, 2U, 31U)},
      engine::ProgramWord{8U, bitwise_xor(0U, 31U, 6U)},
      engine::ProgramWord{12U, system_call},
  };
  const std::array regions{
      engine::MemoryRegion{
          .address = structure::data_base,
          .initial = memory,
          .expected = expected_memory,
      },
  };
  harness.check_program({
      .name = name,
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = structure::native_requirement(),
      .memory = std::span<const engine::MemoryRegion>{regions},
      .expected_instructions = code.size(),
  });
}

void check_single_dependency(engine::EngineHarness &harness,
                             std::string_view name, bool full_vector,
                             bool load, unsigned opcode,
                             unsigned scale_bit, unsigned size,
                             structure::AddressMode mode,
                             std::span<const std::byte> memory,
                             std::uint64_t seed) {
  const auto form = structure::decode_single(
      full_vector, load, 0U, opcode, scale_bit, size);
  engine::require(form.valid && form.register_count == 1U);
  const auto address =
      structure::data_base + structure::page_size / 2U;
  auto initial = structure::seeded_state(seed);
  structure::set_base(initial, 2U, address);
  initial.registers[5] = 29U;
  auto expected = initial;
  auto expected_memory =
      std::vector<std::byte>(memory.begin(), memory.end());
  apply_bitwise_xor(expected, 31U, 3U, 4U);
  const auto before_memory = expected;
  structure::apply_single_oracle(
      before_memory, expected, memory, expected_memory,
      static_cast<std::size_t>(address - structure::data_base), form,
      full_vector, load, 31U);
  structure::apply_writeback(
      before_memory, expected, mode, 2U, 5U,
      form.register_count * form.element_size);
  apply_bitwise_xor(expected, 0U, 31U, 6U);
  expected.pc = engine::engine_test_code_base + 16U;

  const std::array code{
      engine::ProgramWord{0U, bitwise_xor(31U, 3U, 4U)},
      engine::ProgramWord{
          4U,
          structure::single_instruction(
              full_vector, load, structure::post_index(mode), 0U,
              structure::encoded_offset_register(mode, 5U), opcode,
              scale_bit, size, 2U, 31U)},
      engine::ProgramWord{8U, bitwise_xor(0U, 31U, 6U)},
      engine::ProgramWord{12U, system_call},
  };
  const std::array regions{
      engine::MemoryRegion{
          .address = structure::data_base,
          .initial = memory,
          .expected = expected_memory,
      },
  };
  harness.check_program({
      .name = name,
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = structure::native_requirement(),
      .memory = std::span<const engine::MemoryRegion>{regions},
      .expected_instructions = code.size(),
  });
}

void producer_consumer_chains(engine::EngineHarness &harness,
                              std::span<const std::byte> memory,
                              std::uint64_t &seed) {
  check_full_dependency(
      harness, "jit-structure-full-store-dependent", true, false, 0U,
      structure::AddressMode::no_offset, memory, seed++);
  check_full_dependency(
      harness, "jit-structure-full-load-dependent", false, true, 3U,
      structure::AddressMode::post_immediate, memory, seed++);
  check_single_dependency(
      harness, "jit-structure-lane-store-dependent", true, false, 4U, 0U,
      1U, structure::AddressMode::post_register, memory, seed++);
  check_single_dependency(
      harness, "jit-structure-lane-load-dependent", true, true, 4U, 1U,
      0U, structure::AddressMode::no_offset, memory, seed++);
  check_single_dependency(
      harness, "jit-structure-replicate-load-dependent", false, true, 6U,
      0U, 1U, structure::AddressMode::post_immediate, memory, seed++);
}

constexpr std::array cache_fillers{
    bitwise_xor(0U, 1U, 2U),    bitwise_xor(3U, 4U, 5U),
    bitwise_xor(6U, 7U, 8U),    bitwise_xor(9U, 10U, 11U),
    bitwise_xor(12U, 13U, 14U), bitwise_xor(15U, 0U, 1U),
    bitwise_xor(16U, 17U, 0U),
};

void check_full_cache_case(engine::EngineHarness &harness,
                           std::string_view name,
                           std::uint32_t memory_instruction,
                           const structure::SingleForm *single_form,
                           bool full_load, bool full_store,
                           std::span<const std::byte> memory,
                           std::uint64_t seed) {
  const auto address =
      structure::data_base + structure::page_size / 2U;
  auto initial = structure::seeded_state(seed);
  structure::set_base(initial, 2U, address);
  auto expected = initial;
  auto expected_memory =
      std::vector<std::byte>(memory.begin(), memory.end());
  for (const auto filler : cache_fillers) {
    const auto destination = filler & 31U;
    const auto left = (filler >> 5U) & 31U;
    const auto right = (filler >> 16U) & 31U;
    apply_bitwise_xor(expected, destination, left, right);
  }
  const auto before_memory = expected;
  const auto memory_offset =
      static_cast<std::size_t>(address - structure::data_base);
  if (single_form != nullptr) {
    structure::apply_single_oracle(
        before_memory, expected, memory, expected_memory, memory_offset,
        *single_form, true, true, 31U);
  } else {
    const auto &form = structure::multiple_forms.front();
    structure::apply_multiple_oracle(
        before_memory, expected, memory, expected_memory, memory_offset,
        form, true, 0U, full_load, 31U);
    engine::require(full_load != full_store);
  }

  std::vector<engine::ProgramWord> code;
  code.reserve(cache_fillers.size() + 2U);
  for (std::size_t index = 0; index < cache_fillers.size(); ++index)
    code.push_back({index * 4U, cache_fillers[index]});
  code.push_back({cache_fillers.size() * 4U, memory_instruction});
  code.push_back({(cache_fillers.size() + 1U) * 4U, system_call});
  expected.pc =
      engine::engine_test_code_base + code.size() * sizeof(std::uint32_t);
  const std::array regions{
      engine::MemoryRegion{
          .address = structure::data_base,
          .initial = memory,
          .expected = expected_memory,
      },
  };
  harness.check_program({
      .name = name,
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = structure::native_requirement(),
      .memory = std::span<const engine::MemoryRegion>{regions},
      .expected_instructions = code.size(),
  });
}

void full_cache_fallbacks(engine::EngineHarness &harness,
                          std::span<const std::byte> memory,
                          std::uint64_t &seed) {
  const auto lane = structure::decode_single(
      true, true, 0U, 4U, 1U, 0U);
  engine::require(lane.valid && lane.register_count == 1U);
  check_full_cache_case(
      harness, "jit-structure-full-cache-lane-load",
      structure::single_instruction(
          true, true, false, 0U, 0U, 4U, 1U, 0U, 2U, 31U),
      &lane, false, false, memory, seed++);

  const auto &form = structure::multiple_forms.front();
  check_full_cache_case(
      harness, "jit-structure-full-cache-full-store",
      structure::multiple_instruction(
          true, false, false, 0U, form.opcode, 0U, 2U, 31U),
      nullptr, false, true, memory, seed++);
}

void cross_page_success(engine::EngineHarness &harness,
                        std::uint64_t &seed) {
  std::vector<std::byte> memory(2U * structure::page_size);
  structure::fill_memory(memory, 0xd21U);
  const auto boundary = structure::data_base + structure::page_size;
  const auto &full = structure::multiple_forms.front();
  for (const bool load : {false, true})
    structure::check_multiple_success(
        harness,
        load ? "jit-structure-full-cross-page-load"
             : "jit-structure-full-cross-page-store",
        full, true, 0U, load, structure::AddressMode::post_immediate,
        2U, 31U, 5U, boundary - 8U, memory, seed++);

  for (const bool load : {false, true}) {
    const auto lane =
        structure::decode_single(true, load, 0U, 4U, 0U, 1U);
    engine::require(lane.valid && lane.element_size == 8U &&
                    lane.register_count == 1U);
    structure::check_single_success(
        harness,
        load ? "jit-structure-lane-cross-page-load"
             : "jit-structure-lane-cross-page-store",
        lane, true, load, 0U, 4U, 0U, 1U,
        structure::AddressMode::post_register, 2U, 31U, 5U,
        boundary - 4U, memory, seed++);
  }

  const auto replicate =
      structure::decode_single(true, true, 0U, 6U, 0U, 3U);
  engine::require(replicate.valid && replicate.element_size == 8U &&
                  replicate.register_count == 1U);
  structure::check_single_success(
      harness, "jit-structure-replicate-cross-page-load", replicate,
      true, true, 0U, 6U, 0U, 3U,
      structure::AddressMode::no_offset, 2U, 31U, 5U,
      boundary - 4U, memory, seed++);
}

void check_cross_page_fault(
    engine::EngineHarness &harness, std::string_view name,
    std::uint32_t instruction, bool load,
    std::span<const std::byte> memory, std::uint64_t seed) {
  const auto boundary = structure::data_base + structure::page_size;
  auto initial = structure::seeded_state(seed);
  structure::set_base(initial, 2U, boundary - 4U);
  structure::check_memory(
      harness, name, instruction, initial, initial, memory, {},
      engine::ExpectedEvent::memory_fault,
      {
          .address = boundary,
          .access = load ? arm64::MemoryAccess::read
                         : arm64::MemoryAccess::write,
      },
      true);
}

void cross_page_and_permission_faults(engine::EngineHarness &harness,
                                      std::uint64_t &seed) {
  std::vector<std::byte> memory(structure::page_size);
  structure::fill_memory(memory, 0xe32U);
  const auto &full = structure::multiple_forms.front();
  for (const bool load : {false, true})
    check_cross_page_fault(
        harness,
        load ? "jit-structure-full-cross-page-fault-load"
             : "jit-structure-full-cross-page-fault-store",
        structure::multiple_instruction(
            true, load, true, 31U, full.opcode, 0U, 2U, 31U),
        load, memory, seed++);

  for (const bool load : {false, true})
    check_cross_page_fault(
        harness,
        load ? "jit-structure-lane-cross-page-fault-load"
             : "jit-structure-lane-cross-page-fault-store",
        structure::single_instruction(
            true, load, true, 0U, 31U, 4U, 0U, 1U, 2U, 31U),
        load, memory, seed++);

  check_cross_page_fault(
      harness, "jit-structure-replicate-cross-page-fault",
      structure::single_instruction(
          true, true, true, 0U, 31U, 6U, 0U, 3U, 2U, 31U),
      true, memory, seed++);

  const auto address =
      structure::data_base + structure::page_size / 2U;
  for (const bool load : {false, true}) {
    auto initial = structure::seeded_state(seed++);
    structure::set_base(initial, 2U, address);
    const std::array regions{
        engine::MemoryRegion{
            .address = structure::data_base,
            .initial = memory,
            .readable = !load,
            .writable = load,
        },
    };
    harness.check({
        .name = load ? "jit-structure-one-register-read-permission"
                     : "jit-structure-one-register-write-permission",
        .instruction = structure::multiple_instruction(
            true, load, false, 0U, full.opcode, 0U, 2U, 31U),
        .initial = initial,
        .expected = initial,
        .event = engine::ExpectedEvent::memory_fault,
        .native = structure::native_requirement(),
        .fault =
            {
                .address = address,
                .access = load ? arm64::MemoryAccess::read
                               : arm64::MemoryAccess::write,
            },
        .compare_fault = true,
        .memory = std::span<const engine::MemoryRegion>{regions},
    });
  }
}

void dirty_producer_is_precise_on_fault(engine::EngineHarness &harness,
                                        std::uint64_t seed) {
  const auto address = structure::data_base;
  auto initial = structure::seeded_state(seed);
  structure::set_base(initial, 2U, address);
  auto expected = initial;
  apply_bitwise_xor(expected, 31U, 3U, 4U);
  expected.pc = engine::engine_test_code_base + 4U;
  const auto lane =
      structure::decode_single(true, true, 0U, 4U, 1U, 0U);
  engine::require(lane.valid && lane.register_count == 1U);
  const std::array code{
      engine::ProgramWord{0U, bitwise_xor(31U, 3U, 4U)},
      engine::ProgramWord{
          4U,
          structure::single_instruction(
              true, true, false, 0U, 0U, 4U, 1U, 0U, 2U, 31U)},
      engine::ProgramWord{8U, system_call},
  };
  harness.check_program({
      .name = "jit-structure-dirty-producer-memory-fault",
      .code = code,
      .initial = initial,
      .expected = expected,
      .event = engine::ExpectedEvent::memory_fault,
      .native = structure::native_requirement(),
      .fault =
          {
              .address = address,
              .access = arm64::MemoryAccess::read,
          },
      .compare_fault = true,
      .expected_instructions = 1U,
  });
}

[[nodiscard]] constexpr std::uint32_t
exclusive(bool load, unsigned base, unsigned transfer,
          unsigned status = 31U) noexcept {
  return 0xc800'0000U | (static_cast<unsigned>(load) << 22U) |
         ((status & 31U) << 16U) | (31U << 10U) |
         ((base & 31U) << 5U) | (transfer & 31U);
}

[[nodiscard]] std::uint64_t
load_u64(std::span<const std::byte> memory, std::size_t offset) {
  std::uint64_t value{};
  for (unsigned byte = 0; byte < 8U; ++byte)
    value |= static_cast<std::uint64_t>(
                 std::to_integer<unsigned>(memory[offset + byte]))
             << (byte * 8U);
  return value;
}

void validate_failed_exclusive(
    const arm64::CpuState &, const arm64::CpuState &expected,
    const arm64::CpuState &actual, std::string_view case_name,
    std::string_view engine_name, std::uint32_t instruction) {
  auto normalized = actual;
  normalized.exclusive_value = expected.exclusive_value;
  normalized.exclusive_value2 = expected.exclusive_value2;
  normalized.exclusive_version = expected.exclusive_version;
  engine::require_architecture_equal(
      expected, normalized, case_name, engine_name, instruction);
}

void check_monitor_clear(engine::EngineHarness &harness,
                         std::string_view name,
                         std::uint32_t store_instruction,
                         bool lane,
                         std::span<const std::byte> memory,
                         std::uint64_t seed) {
  const auto address =
      structure::data_base + structure::page_size / 2U;
  auto initial = structure::seeded_state(seed);
  structure::set_base(initial, 2U, address);
  structure::set_base(initial, 3U, address);
  auto expected = initial;
  auto expected_memory =
      std::vector<std::byte>(memory.begin(), memory.end());
  expected.registers[5] = load_u64(
      memory, static_cast<std::size_t>(
                  address - structure::data_base));
  expected.registers[6] = 1U;
  expected.exclusive_address = ~std::uint64_t{0};
  expected.exclusive_key = 0U;
  expected.exclusive_size = 0U;
  if (lane) {
    const auto form =
        structure::decode_single(true, false, 0U, 4U, 0U, 1U);
    engine::require(form.valid && form.register_count == 1U);
    structure::apply_single_oracle(
        initial, expected, memory, expected_memory,
        static_cast<std::size_t>(address - structure::data_base), form,
        true, false, 31U);
  } else {
    structure::apply_multiple_oracle(
        initial, expected, memory, expected_memory,
        static_cast<std::size_t>(address - structure::data_base),
        structure::multiple_forms.front(), true, 0U, false, 31U);
  }
  expected.pc = engine::engine_test_code_base + 16U;

  const std::array code{
      engine::ProgramWord{0U, exclusive(true, 2U, 5U)},
      engine::ProgramWord{4U, store_instruction},
      engine::ProgramWord{8U, exclusive(false, 2U, 7U, 6U)},
      engine::ProgramWord{12U, system_call},
  };
  const std::array regions{
      engine::MemoryRegion{
          .address = structure::data_base,
          .initial = memory,
          .expected = expected_memory,
      },
  };
  harness.check_program({
      .name = name,
      .code = code,
      .initial = initial,
      .expected = expected,
      .native = structure::native_requirement(),
      .memory = std::span<const engine::MemoryRegion>{regions},
      .validator = &validate_failed_exclusive,
  });
}

void one_register_stores_clear_the_monitor(
    engine::EngineHarness &harness, std::span<const std::byte> memory,
    std::uint64_t &seed) {
  const auto &full = structure::multiple_forms.front();
  check_monitor_clear(
      harness, "jit-structure-full-store-clears-monitor",
      structure::multiple_instruction(
          true, false, false, 0U, full.opcode, 0U, 3U, 31U),
      false, memory, seed++);
  check_monitor_clear(
      harness, "jit-structure-lane-store-clears-monitor",
      structure::single_instruction(
          true, false, false, 0U, 0U, 4U, 0U, 1U, 3U, 31U),
      true, memory, seed++);
}

} // namespace

int main() {
  engine::EngineHarness harness;
  std::vector<std::byte> memory(structure::page_size);
  structure::fill_memory(memory, 0xc10U);
  std::uint64_t seed = 0xc900'0000'0000'0000ULL;
  full_form_matrix(harness, memory, seed);
  lane_form_matrix(harness, memory, seed);
  replicate_form_matrix(harness, memory, seed);
  producer_consumer_chains(harness, memory, seed);
  full_cache_fallbacks(harness, memory, seed);
  cross_page_success(harness, seed);
  cross_page_and_permission_faults(harness, seed);
  dirty_producer_is_precise_on_fault(harness, seed++);
  one_register_stores_clear_the_monitor(harness, memory, seed);
  harness.print_summary("arm64-jit-simd-structure-remap-runtime");
  return 0;
}
