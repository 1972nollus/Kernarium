// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_immediate_memory_test_support.hpp"
#include "jit_test_support.hpp"
#include "runtime/jit/jit_executor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <vector>

namespace arm64 = uml::backend::arm64;
namespace engine = uml::backend::tests;
namespace immediate = uml::backend::tests::base_immediate_memory;

namespace {

constexpr auto page_size = std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE};
constexpr auto data_base = engine::engine_test_data_base;
constexpr std::uint32_t system_call = 0xd400'0001U;

[[nodiscard]] constexpr std::uint32_t
unsigned_x(bool load, unsigned immediate, unsigned base,
           unsigned transfer) noexcept {
  return immediate::encode_unsigned(3, load ? 1U : 0U, immediate, base,
                                    transfer);
}

[[nodiscard]] constexpr std::uint32_t
move_wide(unsigned destination, unsigned immediate) noexcept {
  return 0xd280'0000U | ((immediate & 0xffffU) << 5U) |
         (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
compare_x(unsigned left, unsigned right) noexcept {
  return 0xeb00'001fU | ((right & 31U) << 16U) | ((left & 31U) << 5U);
}

[[nodiscard]] constexpr std::uint32_t
load_exclusive_byte(unsigned base, unsigned transfer) noexcept {
  return 0x085f'7c00U | ((base & 31U) << 5U) | (transfer & 31U);
}

[[nodiscard]] constexpr std::uint32_t
store_exclusive_byte(unsigned status, unsigned base,
                     unsigned transfer) noexcept {
  return 0x0800'7c00U | ((status & 31U) << 16U) |
         ((base & 31U) << 5U) | (transfer & 31U);
}

void every_scalar_variant(engine::EngineHarness &harness) {
  for (std::size_t index = 0; index < immediate::variants.size(); ++index) {
    const auto &variant = immediate::variants[index];
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, static_cast<unsigned>(index * 19U));
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(
        0xc100'0000'0000'0000ULL | (index << 8U));
    initial.registers[2] = data_base + page_size / 4U;
    initial.registers[3] = 0x0123'4567'89ab'cdefULL;
    initial.registers[4] = 0xfedc'ba98'7654'3210ULL;
    auto expected = initial;
    const std::array instructions{
        immediate::encode_unsigned(variant.size_code, variant.operation, 3,
                                   2, 3),
        immediate::encode_unsigned(variant.size_code, variant.operation, 1,
                                   2, 4),
    };
    immediate::apply_oracle(variant, immediate::Form::unsigned_offset, 3, 2,
                            3, expected, expected_memory, data_base);
    immediate::apply_oracle(variant, immediate::Form::unsigned_offset, 1, 2,
                            4, expected, expected_memory, data_base);
    char name[96];
    std::snprintf(name, sizeof(name), "jit-gpr-run-%.*s",
                  static_cast<int>(variant.name.size()),
                  variant.name.data());
    immediate::check_program(harness, name, instructions, initial, expected,
                             data_base, memory, expected_memory);
  }
}

void alias_and_order_cases(engine::EngineHarness &harness) {
  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 91);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xc200'0000'0000'0001ULL);
    initial.registers[2] = data_base + page_size / 4U;
    auto expected = initial;
    const std::array instructions{
        immediate::encode_unsigned(0, 1, 0, 2, 3),
        immediate::encode_unsigned(0, 2, 1, 2, 3),
        immediate::encode_unsigned(1, 1, 1, 2, 31),
        immediate::encode_unsigned(2, 2, 1, 2, 4),
    };
    const std::array variant_indices{1U, 2U, 5U, 10U};
    const std::array immediates{0U, 1U, 1U, 1U};
    const std::array transfers{3U, 3U, 31U, 4U};
    for (std::size_t index = 0; index < instructions.size(); ++index)
      immediate::apply_oracle(
          immediate::variants[variant_indices[index]],
          immediate::Form::unsigned_offset, immediates[index], 2,
          transfers[index], expected, expected_memory, data_base);
    immediate::check_program(
        harness, "jit-gpr-run-load-aliases", instructions, initial, expected,
        data_base, memory, expected_memory);
  }

  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 109);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xc200'0000'0000'0002ULL);
    initial.registers[2] = data_base + page_size / 4U;
    initial.registers[3] = 0x1122'3344'5566'7788ULL;
    initial.registers[4] = 0xa5;
    initial.registers[5] = 0xbeef;
    auto expected = initial;
    const std::array instructions{
        immediate::encode_unsigned(3, 0, 0, 2, 2),
        immediate::encode_unsigned(0, 0, 3, 2, 4),
        immediate::encode_unsigned(1, 0, 1, 2, 5),
        immediate::encode_unsigned(2, 0, 1, 2, 31),
    };
    const std::array variant_indices{11U, 0U, 4U, 8U};
    const std::array immediates{0U, 3U, 1U, 1U};
    const std::array transfers{2U, 4U, 5U, 31U};
    for (std::size_t index = 0; index < instructions.size(); ++index)
      immediate::apply_oracle(
          immediate::variants[variant_indices[index]],
          immediate::Form::unsigned_offset, immediates[index], 2,
          transfers[index], expected, expected_memory, data_base);
    immediate::check_program(
        harness, "jit-gpr-run-store-base-alias", instructions, initial,
        expected, data_base, memory, expected_memory);
  }

  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 113);
    constexpr std::size_t first_offset = 64;
    constexpr std::size_t second_offset = 160;
    const auto second_base = data_base + second_offset;
    immediate::encode_little_endian(memory, first_offset, 8, second_base);
    constexpr std::uint64_t second_value = 0x8877'6655'4433'2211ULL;
    immediate::encode_little_endian(memory, second_offset + 8U, 8,
                                    second_value);
    const auto expected_memory = memory;
    arm64::CpuState initial{};
    initial.registers[2] = data_base + first_offset;
    initial.registers[3] = 0x1111;
    auto expected = initial;
    expected.registers[2] = second_base;
    expected.registers[3] = second_value;
    const std::array instructions{
        unsigned_x(true, 0, 2, 2),
        unsigned_x(true, 1, 2, 3),
    };
    immediate::check_program(
        harness, "jit-gpr-run-load-base-alias-changes-next-address",
        instructions, initial, expected, data_base, memory, expected_memory);
  }
}

void infrastructure_guest_register_cases(engine::EngineHarness &harness) {
  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 117);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xc250'0000'0000'0001ULL);
    initial.registers[8] = data_base + page_size / 4U;
    auto expected = initial;
    const std::array instructions{
        unsigned_x(true, 0, 8, 9),
        unsigned_x(true, 1, 8, 16),
        unsigned_x(true, 2, 8, 17),
        unsigned_x(true, 3, 8, 18),
    };
    constexpr std::array transfers{9U, 16U, 17U, 18U};
    for (std::size_t index = 0; index < instructions.size(); ++index)
      immediate::apply_oracle(
          immediate::variants[12], immediate::Form::unsigned_offset,
          static_cast<unsigned>(index), 8, transfers[index], expected,
          expected_memory, data_base);
    immediate::check_program(
        harness, "jit-gpr-run-infrastructure-register-loads", instructions,
        initial, expected, data_base, memory, expected_memory);
  }

  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 133);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xc250'0000'0000'0002ULL);
    initial.registers[2] = data_base + page_size / 4U;
    initial.registers[8] = 0x0123'4567'89ab'cdefULL;
    initial.registers[9] = 0xfedc'ba98'7654'3210ULL;
    auto expected = initial;
    const std::array instructions{
        unsigned_x(false, 0, 2, 8),
        unsigned_x(false, 1, 2, 9),
    };
    immediate::apply_oracle(immediate::variants[11],
                            immediate::Form::unsigned_offset, 0, 2, 8,
                            expected, expected_memory, data_base);
    immediate::apply_oracle(immediate::variants[11],
                            immediate::Form::unsigned_offset, 1, 2, 9,
                            expected, expected_memory, data_base);
    immediate::check_program(
        harness, "jit-gpr-run-infrastructure-register-stores", instructions,
        initial, expected, data_base, memory, expected_memory);
  }
}

void all_guest_register_cases(engine::EngineHarness &harness) {
  for (unsigned base = 0; base < 31U; ++base) {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 151U + base);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(
        0xc280'0000'0000'0000ULL | base);
    initial.registers[base] = data_base + page_size / 4U;
    auto expected = initial;
    const unsigned first = (base + 1U) % 31U;
    const unsigned second = (base + 2U) % 31U;
    const std::array instructions{
        unsigned_x(true, 0, base, first),
        unsigned_x(true, 1, base, second),
    };
    immediate::apply_oracle(
        immediate::variants[12], immediate::Form::unsigned_offset, 0,
        base, first, expected, expected_memory, data_base);
    immediate::apply_oracle(
        immediate::variants[12], immediate::Form::unsigned_offset, 1,
        base, second, expected, expected_memory, data_base);
    char name[80];
    std::snprintf(name, sizeof(name),
                  "jit-gpr-run-all-base-registers-r%u", base);
    immediate::check_program(harness, name, instructions, initial, expected,
                             data_base, memory, expected_memory);
  }

  for (unsigned transfer = 0; transfer < 32U; ++transfer) {
    for (const bool load : {false, true}) {
      std::vector<std::byte> memory(page_size);
      immediate::fill_memory(
          memory, 193U + transfer * 3U + static_cast<unsigned>(load));
      auto expected_memory = memory;
      auto initial = immediate::seeded_state(
          0xc290'0000'0000'0000ULL |
          (static_cast<std::uint64_t>(transfer) << 1U) |
          static_cast<unsigned>(load));
      const unsigned base = transfer == 2U ? 3U : 2U;
      initial.registers[base] = data_base + page_size / 4U;
      auto expected = initial;
      const std::array instructions{
          unsigned_x(load, 0, base, transfer),
          unsigned_x(load, 1, base, transfer),
      };
      const auto &variant =
          immediate::variants[load ? 12U : 11U];
      immediate::apply_oracle(
          variant, immediate::Form::unsigned_offset, 0, base, transfer,
          expected, expected_memory, data_base);
      immediate::apply_oracle(
          variant, immediate::Form::unsigned_offset, 1, base, transfer,
          expected, expected_memory, data_base);
      char name[96];
      std::snprintf(
          name, sizeof(name), "jit-gpr-run-all-transfer-registers-%s-r%u",
          load ? "load" : "store", transfer);
      immediate::check_program(
          harness, name, instructions, initial, expected, data_base, memory,
          expected_memory);
    }
  }
}

void lengths_two_through_six(engine::EngineHarness &harness) {
  for (std::size_t length = 2; length <= 6; ++length) {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, static_cast<unsigned>(length * 31U));
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(
        0xc300'0000'0000'0000ULL | length);
    initial.registers[2] = data_base + page_size / 4U;
    auto expected = initial;
    std::vector<std::uint32_t> instructions;
    instructions.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
      const auto immediate_value =
          static_cast<unsigned>((length - index) * 2U);
      const auto transfer = static_cast<unsigned>(8U + index);
      instructions.push_back(
          unsigned_x(true, immediate_value, 2, transfer));
      immediate::apply_oracle(
          immediate::variants[12], immediate::Form::unsigned_offset,
          immediate_value, 2, transfer, expected, expected_memory, data_base);
    }
    char name[64];
    std::snprintf(name, sizeof(name), "jit-gpr-run-length-%zu", length);
    immediate::check_program(harness, name, instructions, initial, expected,
                             data_base, memory, expected_memory);
  }
}

void exact_page_end_and_runtime_crossing(engine::EngineHarness &harness) {
  constexpr unsigned last_x = (DARWIN_UML_GUEST_PAGE_SIZE - 8U) / 8U;
  const std::array instructions{
      unsigned_x(true, 0, 2, 3),
      unsigned_x(true, last_x, 2, 4),
  };

  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 141);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xc400'0000'0000'0001ULL);
    initial.registers[2] = data_base;
    auto expected = initial;
    immediate::apply_oracle(immediate::variants[12],
                            immediate::Form::unsigned_offset, 0, 2, 3,
                            expected, expected_memory, data_base);
    immediate::apply_oracle(immediate::variants[12],
                            immediate::Form::unsigned_offset, last_x, 2, 4,
                            expected, expected_memory, data_base);
    immediate::check_program(
        harness, "jit-gpr-run-exact-page-end", instructions, initial,
        expected, data_base, memory, expected_memory);
  }

  {
    std::vector<std::byte> memory(2U * page_size);
    immediate::fill_memory(memory, 157);
    auto expected_memory = memory;
    auto initial = immediate::seeded_state(0xc400'0000'0000'0002ULL);
    initial.registers[2] = data_base + 16U;
    auto expected = initial;
    immediate::apply_oracle(immediate::variants[12],
                            immediate::Form::unsigned_offset, 0, 2, 3,
                            expected, expected_memory, data_base);
    immediate::apply_oracle(immediate::variants[12],
                            immediate::Form::unsigned_offset, last_x, 2, 4,
                            expected, expected_memory, data_base);
    immediate::check_program(
        harness, "jit-gpr-run-runtime-page-crossing", instructions, initial,
        expected, data_base, memory, expected_memory);
  }

  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 173);
    auto expected_memory = memory;
    arm64::CpuState initial{};
    initial.registers[2] =
        std::numeric_limits<std::uint64_t>::max() - 7U;
    auto expected = initial;
    const std::array wrapping{
        unsigned_x(true, 1, 2, 3),
        unsigned_x(true, 2, 2, 4),
    };
    immediate::apply_oracle(
        immediate::variants[12], immediate::Form::unsigned_offset, 1, 2, 3,
        expected, expected_memory, 0);
    immediate::apply_oracle(
        immediate::variants[12], immediate::Form::unsigned_offset, 2, 2, 4,
        expected, expected_memory, 0);
    immediate::check_program(
        harness, "jit-gpr-run-uint64-address-wrap", wrapping, initial,
        expected, 0, memory, expected_memory);
  }
}

void precise_fault_cases(engine::EngineHarness &harness) {
  {
    const std::array code{
        engine::ProgramWord{.offset = 0,
                            .instruction = unsigned_x(true, 3, 2, 3)},
        engine::ProgramWord{.offset = 4,
                            .instruction = unsigned_x(true, 4, 2, 4)},
        engine::ProgramWord{.offset = 8, .instruction = system_call},
    };
    arm64::CpuState initial{};
    initial.registers[2] = data_base + 8U * page_size;
    initial.registers[3] = 0x1111'2222'3333'4444ULL;
    initial.registers[4] = 0x5555'6666'7777'8888ULL;
    auto expected = initial;
    expected.pc = engine::engine_test_code_base;
    harness.check_program({
        .name = "jit-gpr-run-first-translation-fault-address",
        .code = code,
        .initial = initial,
        .expected = expected,
        .event = engine::ExpectedEvent::memory_fault,
        .native = engine::NativeRequirements::baseline(),
        .fault =
            {
                .address = initial.registers[2] + 24U,
                .access = arm64::MemoryAccess::read,
            },
        .compare_fault = true,
        .expected_instructions = 0,
    });
  }

  {
    constexpr unsigned last_x =
        (DARWIN_UML_GUEST_PAGE_SIZE - 8U) / 8U;
    const std::array code{
        engine::ProgramWord{.offset = 0,
                            .instruction = unsigned_x(false, 0, 2, 3)},
        engine::ProgramWord{.offset = 4,
                            .instruction = unsigned_x(false, last_x, 2, 4)},
        engine::ProgramWord{.offset = 8, .instruction = system_call},
    };
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 173);
    auto expected_memory = memory;
    arm64::CpuState initial{};
    initial.registers[2] = data_base + 16U;
    initial.registers[3] = 0x0123'4567'89ab'cdefULL;
    initial.registers[4] = 0xfedc'ba98'7654'3210ULL;
    auto expected = initial;
    immediate::encode_little_endian(expected_memory, 16, 8,
                                    initial.registers[3]);
    expected.pc = engine::engine_test_code_base + 4U;
    const std::array regions{
        engine::MemoryRegion{
            .address = data_base,
            .initial = memory,
            .expected = expected_memory,
        },
    };
    harness.check_program({
        .name = "jit-gpr-run-later-fault-preserves-first-store",
        .code = code,
        .initial = initial,
        .expected = expected,
        .event = engine::ExpectedEvent::memory_fault,
        .native = engine::NativeRequirements::baseline(),
        .fault =
            {
                .address = data_base + page_size + 8U,
                .access = arm64::MemoryAccess::write,
            },
        .compare_fault = true,
        .memory = regions,
        .expected_instructions = 1,
    });
  }

  {
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 189);
    const std::array read_only{
        engine::MemoryRegion{
            .address = data_base,
            .initial = memory,
            .expected = memory,
            .readable = true,
            .writable = false,
        },
    };
    const std::array code{
        engine::ProgramWord{.offset = 0,
                            .instruction = unsigned_x(false, 2, 2, 3)},
        engine::ProgramWord{.offset = 4,
                            .instruction = unsigned_x(false, 3, 2, 4)},
        engine::ProgramWord{.offset = 8, .instruction = system_call},
    };
    arm64::CpuState initial{};
    initial.registers[2] = data_base;
    initial.registers[3] = 0x1122'3344'5566'7788ULL;
    initial.registers[4] = 0x99aa'bbcc'ddee'ff00ULL;
    auto expected = initial;
    expected.pc = engine::engine_test_code_base;
    harness.check_program({
        .name = "jit-gpr-run-first-store-permission-fault",
        .code = code,
        .initial = initial,
        .expected = expected,
        .event = engine::ExpectedEvent::memory_fault,
        .native = engine::NativeRequirements::baseline(),
        .fault =
            {
                .address = data_base + 16U,
                .access = arm64::MemoryAccess::write,
            },
        .compare_fault = true,
        .memory = read_only,
        .expected_instructions = 0,
    });
  }
}

void prefixed_cold_faults_preserve_state(engine::EngineHarness &harness) {
  {
    const std::array code{
        engine::ProgramWord{.offset = 0,
                            .instruction = move_wide(6, 0x123U)},
        engine::ProgramWord{.offset = 4,
                            .instruction = compare_x(1, 3)},
        engine::ProgramWord{.offset = 8,
                            .instruction = unsigned_x(true, 0, 10, 4)},
        engine::ProgramWord{.offset = 12,
                            .instruction = unsigned_x(true, 1, 10, 5)},
        engine::ProgramWord{.offset = 16, .instruction = system_call},
    };
    auto initial = immediate::seeded_state(0xc500'0000'0000'0001ULL);
    initial.registers[1] = 7;
    initial.registers[3] = 7;
    initial.registers[10] = data_base + 8U * page_size;
    const auto untouched_first = initial.registers[4];
    const auto untouched_second = initial.registers[5];
    auto expected = initial;
    expected.registers[6] = 0x123U;
    expected.registers[4] = untouched_first;
    expected.registers[5] = untouched_second;
    expected.nzcv = 0x6000'0000U;
    expected.pc = engine::engine_test_code_base + 8U;
    harness.check_program({
        .name = "jit-gpr-run-prefixed-translation-cold-fault",
        .code = code,
        .initial = initial,
        .expected = expected,
        .event = engine::ExpectedEvent::memory_fault,
        .native = engine::NativeRequirements::baseline(),
        .fault =
            {
                .address = initial.registers[10],
                .access = arm64::MemoryAccess::read,
            },
        .compare_fault = true,
        .expected_instructions = 2,
    });
  }

  {
    const std::array code{
        engine::ProgramWord{.offset = 0,
                            .instruction = move_wide(6, 0x456U)},
        engine::ProgramWord{.offset = 4,
                            .instruction = compare_x(1, 3)},
        engine::ProgramWord{.offset = 8,
                            .instruction = unsigned_x(false, 2, 10, 4)},
        engine::ProgramWord{.offset = 12,
                            .instruction = unsigned_x(false, 3, 10, 5)},
        engine::ProgramWord{.offset = 16, .instruction = system_call},
    };
    std::vector<std::byte> memory(page_size);
    immediate::fill_memory(memory, 213);
    const std::array read_only{
        engine::MemoryRegion{
            .address = data_base,
            .initial = memory,
            .expected = memory,
            .readable = true,
            .writable = false,
        },
    };
    auto initial = immediate::seeded_state(0xc500'0000'0000'0002ULL);
    initial.registers[1] = 11;
    initial.registers[3] = 11;
    initial.registers[10] = data_base;
    auto expected = initial;
    expected.registers[6] = 0x456U;
    expected.nzcv = 0x6000'0000U;
    expected.pc = engine::engine_test_code_base + 8U;
    harness.check_program({
        .name = "jit-gpr-run-prefixed-permission-cold-fault",
        .code = code,
        .initial = initial,
        .expected = expected,
        .event = engine::ExpectedEvent::memory_fault,
        .native = engine::NativeRequirements::baseline(),
        .fault =
            {
                .address = data_base + 16U,
                .access = arm64::MemoryAccess::write,
            },
        .compare_fault = true,
        .memory = read_only,
        .expected_instructions = 2,
    });
  }
}

struct Run final {
  arm64::gadgets::Event event{};
  std::size_t executed{};
};

[[nodiscard]] Run run_once(uml::backend::JitExecutor &executor,
                           uml::backend::StaticGuestMemory &memory,
                           unsigned long generation, arm64::CpuState &cpu) {
  engine::require(executor.begin_execution(generation, cpu.pc));
  std::size_t executed = 0;
  const auto event = executor.run_block(cpu, memory, generation, executed);
  executor.end_execution();
  return {event, executed};
}

void remapped_page_uses_new_backing() {
  constexpr unsigned long generation = 9401;
  constexpr std::uint64_t base = 0xd0'0000;
  constexpr std::uint64_t old_first = 0x1111'2222'3333'4444ULL;
  constexpr std::uint64_t old_second = 0x5555'6666'7777'8888ULL;
  constexpr std::uint64_t new_first = 0x9999'aaaa'bbbb'ccccULL;
  constexpr std::uint64_t new_second = 0xdddd'eeee'ffff'0001ULL;
  engine::JitBacking original{2};
  engine::JitBacking replacement{1};
  original.put32(0, unsigned_x(true, 0, 2, 3));
  original.put32(4, unsigned_x(true, 1, 2, 4));
  original.put32(8, system_call);
  original.put64(page_size, old_first);
  original.put64(page_size + 8U, old_second);
  replacement.put64(0, new_first);
  replacement.put64(8, new_second);
  engine::JitMapping mapping{generation, base, original, 2};
  uml::backend::JitExecutor executor;
  uml::backend::StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[2] = base + page_size;

  auto run = run_once(executor, memory, generation, cpu);
  engine::require(run.event == arm64::gadgets::Event::system_call);
  engine::require(run.executed == 3);
  engine::require(cpu.registers[3] == old_first &&
                  cpu.registers[4] == old_second);

  engine::require(darwin_uml_static_vm_rebind(
                      base + page_size, replacement.descriptor(), 0) == 0);
  cpu.pc = base;
  cpu.registers[3] = 0;
  cpu.registers[4] = 0;
  run = run_once(executor, memory, generation, cpu);
  engine::require(run.event == arm64::gadgets::Event::system_call);
  engine::require(run.executed == 3);
  engine::require(cpu.registers[3] == new_first &&
                  cpu.registers[4] == new_second);
}

enum class ReservationTarget : unsigned {
  first,
  second,
  nonoverlap,
};

void tracked_store_reservation_case(unsigned size_shift,
                                    ReservationTarget target) {
  const auto case_index =
      size_shift * 3U + static_cast<unsigned>(target);
  const auto generation = static_cast<unsigned long>(9500U + case_index);
  const auto base =
      std::uint64_t{0xe0'0000} + case_index * 4U * page_size;
  constexpr std::size_t first_offset = 16;
  constexpr std::size_t second_offset = 32;
  constexpr std::size_t nonoverlap_offset = 64;
  const auto bytes = std::size_t{1} << size_shift;
  const auto reservation_offset =
      target == ReservationTarget::first
          ? first_offset + bytes - 1U
      : target == ReservationTarget::second
          ? second_offset + bytes - 1U
          : nonoverlap_offset;
  constexpr std::uint64_t first_value = 0x0123'4567'89ab'cda5ULL;
  constexpr std::uint64_t second_value = 0xfedc'ba98'7654'325aULL;
  constexpr std::uint64_t exclusive_value = 0xc7U;
  engine::JitBacking backing{2};
  backing.put32(0, load_exclusive_byte(1, 5));
  backing.put32(4, system_call);
  backing.put32(
      8, immediate::encode_unsigned(
             size_shift, 0, first_offset >> size_shift, 2, 3));
  backing.put32(
      12, immediate::encode_unsigned(
              size_shift, 0, second_offset >> size_shift, 2, 4));
  backing.put32(16, system_call);
  backing.put32(20, store_exclusive_byte(6, 1, 4));
  backing.put32(24, system_call);
  backing.put64(page_size + first_offset, 0);
  backing.put64(page_size + second_offset, 0);
  backing.put64(page_size + nonoverlap_offset, 0);
  engine::JitMapping mapping{generation, base, backing, 2};

  uml::backend::JitExecutor first;
  uml::backend::JitExecutor second;
  uml::backend::StaticGuestMemory shared_memory;
  arm64::CpuState first_cpu{};
  arm64::CpuState second_cpu{};
  first_cpu.pc = base;
  first_cpu.registers[1] = base + page_size + reservation_offset;
  first_cpu.registers[4] = exclusive_value;
  second_cpu.pc = base + 8U;
  second_cpu.registers[2] = base + page_size;
  second_cpu.registers[3] = first_value;
  second_cpu.registers[4] = second_value;

  engine::require(first.begin_execution(generation, first_cpu.pc));
  std::size_t executed = 0;
  engine::require(first.run_block(first_cpu, shared_memory, generation,
                                  executed) ==
                  arm64::gadgets::Event::continued);
  engine::require(executed == 1);
  engine::require(first_cpu.exclusive_size == 1);

  engine::require(second.begin_execution(generation, second_cpu.pc));
  engine::require(second.run_block(second_cpu, shared_memory, generation,
                                   executed) ==
                  arm64::gadgets::Event::system_call);
  engine::require(executed == 3);
  const auto value_mask =
      bytes == sizeof(std::uint64_t)
          ? ~std::uint64_t{}
          : (std::uint64_t{1} << (bytes * 8U)) - 1U;
  engine::require((backing.get64(page_size + first_offset) & value_mask) ==
                  (first_value & value_mask));
  engine::require((backing.get64(page_size + second_offset) & value_mask) ==
                  (second_value & value_mask));
  second.end_execution();

  first_cpu.pc = base + 20U;
  engine::require(first.run_block(first_cpu, shared_memory, generation,
                                  executed) ==
                  arm64::gadgets::Event::continued);
  engine::require(executed == 1);
  const bool overlaps = target != ReservationTarget::nonoverlap;
  engine::require(first_cpu.registers[6] == (overlaps ? 1U : 0U));
  engine::require(first_cpu.exclusive_size == 0);
  if (!overlaps)
    engine::require(
        (backing.get64(page_size + nonoverlap_offset) & 0xffU) ==
        exclusive_value);
  first.end_execution();
}

void tracked_store_runs_track_every_range() {
  for (unsigned size_shift = 0; size_shift < 4U; ++size_shift)
    for (const auto target : {ReservationTarget::first,
                              ReservationTarget::second,
                              ReservationTarget::nonoverlap})
      tracked_store_reservation_case(size_shift, target);
}

} // namespace

int main() {
  engine::EngineHarness harness;
  every_scalar_variant(harness);
  alias_and_order_cases(harness);
  infrastructure_guest_register_cases(harness);
  all_guest_register_cases(harness);
  lengths_two_through_six(harness);
  exact_page_end_and_runtime_crossing(harness);
  precise_fault_cases(harness);
  prefixed_cold_faults_preserve_state(harness);
  const auto &counts = harness.counts();
  engine::require(counts.interpreter != 0);
  engine::require(counts.gadgets == counts.interpreter);
  engine::require(counts.jit == counts.interpreter);
  engine::require(counts.jit_skipped == 0);
  engine::require(counts.jit_skipped_host_feature == 0);
  engine::require(counts.jit_unavailable == 0);
  harness.print_summary("arm64-jit-gpr-memory-run-runtime");
  remapped_page_uses_new_backing();
  tracked_store_runs_track_every_range();
  return 0;
}
