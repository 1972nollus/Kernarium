// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

using namespace uml::emulator::tests;

extern "C" std::uint64_t
uml_arm64_jit_chain_abi_probe(void *context, std::uint64_t *observed);

namespace {

namespace arm64 = uml::cpu::arm64;
using uml::cpu::arm64::JitExecutor;
using uml::memory::StaticGuestMemory;
using Event = ::uml::cpu::EngineEvent;

constexpr std::uint32_t system_call = 0xd400'0001U;

[[nodiscard]] constexpr std::uint32_t branch(
    std::uint64_t source, std::uint64_t target) noexcept {
  const auto displacement = (static_cast<std::int64_t>(target) -
                             static_cast<std::int64_t>(source)) /
                            4;
  return 0x1400'0000U |
         (static_cast<std::uint32_t>(displacement) & 0x03ff'ffffU);
}

[[nodiscard]] constexpr std::uint32_t
branch_register(unsigned source) noexcept {
  return 0xd61f'0000U | ((source & 31U) << 5U);
}

struct Run final {
  Event event{};
  std::size_t executed{};
};

[[nodiscard]] Run run_block(JitExecutor &executor, StaticGuestMemory &memory,
                            unsigned long generation, arm64::CpuState &cpu) {
  std::size_t executed = 0;
  const auto event =
      executor.run_block(cpu, memory, generation, executed);
  return {event, executed};
}

struct AbiProbeContext final {
  JitExecutor *executor;
  StaticGuestMemory *memory;
  arm64::CpuState *cpu;
  unsigned long generation;
};

void host_callee_saved_registers_are_restored() {
  constexpr unsigned long generation = 8300;
  constexpr std::uint64_t base = 0xdc'0000;
  JitBacking backing{1};
  backing.put32(0, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  require(executor.begin_execution(generation, cpu.pc));

  AbiProbeContext context{&executor, &memory, &cpu, generation};
  std::array<std::uint64_t, 3> observed{};
  const auto packed =
      uml_arm64_jit_chain_abi_probe(&context, observed.data());
  require(static_cast<Event>(static_cast<std::uint32_t>(packed)) ==
          ::uml::cpu::EngineEvent::system_call);
  require((packed >> 32U) == 1);
  require(observed ==
          std::array<std::uint64_t, 3>{0x1919, 0x2020, 0x2121});
  executor.end_execution();
}

void direct_chain_length(std::size_t links, unsigned long generation,
                         std::uint64_t base) {
  JitBacking backing{1};
  for (std::size_t index = 0; index < links; ++index) {
    const auto pc = base + index * 16U;
    const auto target = pc + 16U;
    backing.put32(index * 16U, 0x9100'0400U); // add x0, x0, #1
    backing.put32(index * 16U + 4U, branch(pc + 4U, target));
  }
  const auto terminal = base + links * 16U;
  backing.put32(links * 16U, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = terminal;
  require(executor.begin_execution(generation, cpu.pc));

  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 1);
  for (std::size_t index = links; index-- > 0;) {
    cpu.pc = base + index * 16U;
    run = run_block(executor, memory, generation, cpu);
    require(run.event == ::uml::cpu::EngineEvent::system_call);
  }

  cpu.pc = base;
  cpu.registers[0] = 0;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call);
  require(run.executed == links * 2U + 1U);
  require(cpu.registers[0] == links);
  executor.end_execution();
}

void zero_one_two_three_and_many_links() {
  direct_chain_length(0, 8301, 0xdd'0000);
  direct_chain_length(1, 8302, 0xde'0000);
  direct_chain_length(2, 8303, 0xdf'0000);
  direct_chain_length(3, 8304, 0xe0'0000);
  direct_chain_length(32, 8305, 0xe1'0000);
}

void exact_fairness_commit_boundary() {
  constexpr unsigned long generation = 8306;
  constexpr std::uint64_t base = 0xe2'0000;
  JitBacking backing{1};
  backing.put32(0, branch(base, base));
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  require(executor.begin_execution(generation, cpu.pc));

  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed ==
          uml::cpu::arm64::jit::testing::link_chain_instruction_limit());
  require(cpu.pc == base);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  /*
   * Both invocations reach the final one-instruction block with one remaining
   * slot. Its completed branch is restored into the budget when the next
   * whole block cannot fit, so the returned count remains exactly 4096.
   */
  require(run.executed ==
          uml::cpu::arm64::jit::testing::link_chain_instruction_limit());
  require(cpu.pc == base);
  executor.end_execution();
}

void non_dividing_fairness_quantum_restores_the_source() {
  constexpr unsigned long generation = 8310;
  constexpr std::uint64_t base = 0xe6'0000;
  constexpr std::size_t quantum = 3;
  JitBacking backing{1};
  backing.put32(0, 0x9100'0400U); // add x0, x0, #1
  backing.put32(4, 0x9100'0421U); // add x1, x1, #1
  backing.put32(8, branch(base + 8, base));
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  require(executor.begin_execution(generation, cpu.pc));

  const auto limit =
      uml::cpu::arm64::jit::testing::link_chain_instruction_limit();
  const auto expected = (limit / quantum) * quantum;
  const auto iterations = expected / quantum;
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == expected);
  require(cpu.pc == base);
  require(cpu.registers[0] == iterations);
  require(cpu.registers[1] == iterations);

  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued);
  require(run.executed == expected);
  require(cpu.pc == base);
  require(cpu.registers[0] == iterations * 2U);
  require(cpu.registers[1] == iterations * 2U);
  executor.end_execution();
}

void conditional_and_pic_exits_chain() {
  constexpr unsigned long generation = 8307;
  constexpr std::uint64_t base = 0xe3'0000;
  constexpr auto conditional_target = base + 0x40;
  constexpr auto pic_source = base + 0x80;
  constexpr auto pic_target = base + 0xc0;
  JitBacking backing{1};
  backing.put32(0, 0xf100'0400U); // subs x0, x0, #1
  backing.put32(4, 0x5400'01e1U); // b.ne +0x3c
  backing.put32(8, system_call);
  backing.put32(conditional_target - base, system_call);
  backing.put32(pic_source - base, branch_register(5));
  backing.put32(pic_target - base, 0x9100'0442U); // add x2, x2, #1
  backing.put32(pic_target - base + 4, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base + 8;
  require(executor.begin_execution(generation, cpu.pc));
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);
  cpu.pc = conditional_target;
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);

  cpu.pc = base;
  cpu.registers[0] = 1;
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 3);
  require(cpu.registers[0] == 0);
  require(cpu.nzcv == 0x6000'0000U);
  cpu.pc = base;
  cpu.registers[0] = 2;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 3);
  require(cpu.registers[0] == 1);
  require(cpu.nzcv == 0x2000'0000U);

  cpu.pc = pic_source;
  cpu.registers[5] = pic_target;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 1);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 2);
  cpu.pc = pic_source;
  cpu.registers[2] = 0;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 3);
  require(cpu.registers[2] == 1);
  executor.end_execution();
}

void linked_fault_keeps_precise_count_and_pc() {
  constexpr unsigned long generation = 8308;
  constexpr std::uint64_t base = 0xe4'0000;
  constexpr auto target = base + 0x40;
  JitBacking backing{1};
  backing.put32(0, branch(base, target));
  backing.put32(target - base, 0xf940'0001U); // ldr x1, [x0]
  backing.put32(target - base + 4, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = target;
  cpu.registers[0] = 0x1234'0000;
  require(executor.begin_execution(generation, cpu.pc));
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::memory_fault && run.executed == 0);

  cpu.pc = base;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::memory_fault);
  require(run.executed == 1);
  require(cpu.pc == target);
  require(executor.fault_address() == cpu.registers[0]);
  executor.end_execution();
}

void publication_mode_and_unsafe_targets_remain_isolated() {
  constexpr unsigned long generation = 8309;
  constexpr std::uint64_t base = 0xe5'0000;
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr auto store_target = base + 0x40;
  constexpr auto terminal = base + 0x80;
  constexpr auto published_source = base + 0x100;
  constexpr auto reservation_target = base + 0x140;
  constexpr auto reservation_source = base + 0x180;
  constexpr auto fp_target = base + 0x1c0;
  constexpr auto fp_source = base + 0x200;
  JitBacking backing{2};
  backing.put32(0, branch(base, store_target));
  backing.put32(store_target - base, 0xf900'0001U); // str x1, [x0]
  backing.put32(store_target - base + 4,
                branch(store_target + 4, terminal));
  backing.put32(terminal - base, system_call);
  backing.put32(published_source - base, 0xf900'0001U);
  backing.put32(published_source - base + 4,
                branch(published_source + 4, store_target));
  backing.put32(reservation_target - base, 0xc85f'7c02U); // ldxr x2,[x0]
  backing.put32(reservation_target - base + 4, system_call);
  backing.put32(reservation_source - base,
                branch(reservation_source, reservation_target));
  backing.put32(fp_target - base, 0x9e66'0000U); // fmov x0, d0
  backing.put32(fp_target - base + 4, system_call);
  backing.put32(fp_source - base, branch(fp_source, fp_target));
  JitMapping mapping{generation, base, backing, 2};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.registers[0] = base + page;
  cpu.registers[1] = 0x1122'3344'5566'7788ULL;
  cpu.pc = terminal;
  require(executor.begin_execution(generation, cpu.pc));
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);
  cpu.pc = store_target;
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);

  cpu.pc = base;
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 1);
  require(cpu.pc == store_target);

  cpu.pc = published_source;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 5);
  require(backing.get64(page) == cpu.registers[1]);

  cpu.pc = reservation_target;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 1);
  executor.end_execution();
  cpu.pc = reservation_source;
  require(executor.begin_execution(generation, cpu.pc));
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 1);
  require(cpu.pc == reservation_target);

  cpu.pc = fp_target;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 2);
  cpu.pc = fp_source;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 1);
  require(cpu.pc == fp_target);
  executor.end_execution();
}

} // namespace

extern "C" std::uint64_t
uml_arm64_jit_chain_abi_callback(void *opaque) {
  auto &context = *static_cast<AbiProbeContext *>(opaque);
  const auto run = run_block(*context.executor, *context.memory,
                             context.generation, *context.cpu);
  return static_cast<std::uint32_t>(run.event) |
         (static_cast<std::uint64_t>(run.executed) << 32U);
}

int main() {
  require(jit_available() == 1);
  host_callee_saved_registers_are_restored();
  zero_one_two_three_and_many_links();
  exact_fairness_commit_boundary();
  non_dividing_fairness_quantum_restores_the_source();
  conditional_and_pic_exits_chain();
  linked_fault_keeps_precise_count_and_pc();
  publication_mode_and_unsafe_targets_remain_isolated();
  return 0;
}
