// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "runtime/host_features.hpp"
#include "runtime/jit/jit_executor.hpp"
#include "runtime/jit/jit_test_api.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

using namespace uml::backend::tests;

namespace {

namespace jit_test = uml::backend::jit::testing;
using uml::backend::arm64::gadgets::Event;

constexpr std::uint32_t allocated_tag_add = 0x9180'0020U;
constexpr std::uint32_t guarded_pop = 0xd52b'7720U;
constexpr std::uint32_t unknown_encoding = 0xffff'ffffU;
constexpr std::uint32_t add_x5_one = 0x9100'04a5U;
constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

void require_guest_illegal_compilation(std::uint64_t pc,
                                       std::uint32_t instruction) {
  const std::array code{instruction};
  const auto compiled = jit_test::compile_block(pc, code);
  require(compiled.guest_illegal);
  require(compiled.guest_instructions == 1);
  require(!compiled.words.empty());
}

void first_word_illegal(unsigned long generation, std::uint64_t base,
                        std::uint32_t instruction) {
  JitBacking backing{1};
  backing.put32(0, instruction);
  backing.put32(4, terminal_system_call);
  JitMapping mapping{generation, base, backing, 1};

  uml::backend::JitExecutor executor;
  uml::backend::StaticGuestMemory memory;
  uml::backend::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[0] = 0x1122'3344'5566'7788ULL;
  cpu.registers[1] = 0x8877'6655'4433'2211ULL;
  const auto initial = cpu;

  require(executor.begin_execution(generation, cpu.pc));
  std::size_t executed = 99;
  require(executor.run_block(cpu, memory, generation, executed) ==
          Event::illegal_instruction);
  require(executed == 0);
  require(cpu.pc == base);
  require(cpu.registers == initial.registers);

  cpu.pc = base;
  executed = 99;
  require(executor.run_block(cpu, memory, generation, executed) ==
          Event::illegal_instruction);
  require(executed == 0);
  require(cpu.pc == base);
  require(cpu.registers == initial.registers);
  executor.end_execution();
}

void valid_host_unavailable_is_guest_illegal() {
  if (uml::backend::host_arm_features().has(uml::backend::ArmFeature::gcs)) {
    std::puts("arm64-jit-illegal-runtime: host-unavailable case skipped");
    return;
  }
  constexpr std::uint64_t base = 0x4fa0'0000;
  require_guest_illegal_compilation(base, guarded_pop);
  first_word_illegal(851, base, guarded_pop);
}

void valid_contract_unavailable_is_guest_illegal() {
  constexpr std::uint64_t base = 0x4fb0'0000;
  require_guest_illegal_compilation(base, allocated_tag_add);
  first_word_illegal(852, base, allocated_tag_add);
}

void unknown_word_is_guest_illegal() {
  constexpr std::uint64_t base = 0x4fc0'0000;
  require_guest_illegal_compilation(base, unknown_encoding);
  first_word_illegal(853, base, unknown_encoding);
}

void supported_prefix_stops_before_illegal_word() {
  constexpr unsigned long generation = 854;
  constexpr std::uint64_t base = 0x4fd0'0000;
  constexpr std::array code{
      add_x5_one,
      allocated_tag_add,
      terminal_system_call,
  };
  const auto compiled = jit_test::compile_block(base, code);
  require(!compiled.guest_illegal);
  require(compiled.guest_instructions == 1);

  JitBacking backing{1};
  for (std::size_t index = 0; index < code.size(); ++index)
    backing.put32(index * sizeof(std::uint32_t), code[index]);
  JitMapping mapping{generation, base, backing, 1};

  uml::backend::JitExecutor executor;
  uml::backend::StaticGuestMemory memory;
  uml::backend::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[0] = 0xdead'beef'feed'faceULL;
  cpu.registers[1] = 0x0123'4567'89ab'cdefULL;
  cpu.registers[5] = 41;

  require(executor.begin_execution(generation, cpu.pc));
  std::size_t executed = 99;
  require(executor.run_block(cpu, memory, generation, executed) ==
          Event::continued);
  require(executed == 1);
  require(cpu.pc == base + sizeof(std::uint32_t));
  require(cpu.registers[5] == 42);
  require(cpu.registers[0] == 0xdead'beef'feed'faceULL);

  executed = 99;
  require(executor.run_block(cpu, memory, generation, executed) ==
          Event::illegal_instruction);
  require(executed == 0);
  require(cpu.pc == base + sizeof(std::uint32_t));
  require(cpu.registers[5] == 42);
  require(cpu.registers[0] == 0xdead'beef'feed'faceULL);
  executor.end_execution();
}

} // namespace

int main() {
  require(darwin_uml_jit_available() == 1);
  valid_host_unavailable_is_guest_illegal();
  valid_contract_unavailable_is_guest_illegal();
  unknown_word_is_guest_illegal();
  supported_prefix_stops_before_illegal_word();
  return 0;
}
