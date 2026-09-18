// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_runtime_contract.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "scratch_directory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

using namespace uml::emulator::tests;

namespace {

namespace arm64 = uml::cpu::arm64;
using uml::cpu::arm64::JitExecutor;
using uml::cpu::arm64::JitExecutorDiagnosticsOptions;
using uml::cpu::arm64::JitExecutorOptions;
using uml::memory::StaticGuestMemory;
using Event = ::uml::cpu::EngineEvent;

constexpr std::uint32_t system_call = 0xd400'0001U;

static_assert(!arm64::share_translations_between_executors,
              "profile counter pointers cannot cross executor lifetimes");

[[nodiscard]] constexpr std::uint32_t
add_immediate(unsigned destination, unsigned source,
              unsigned immediate = 1) noexcept {
  return 0x9100'0000U | ((immediate & 0xfffU) << 10U) |
         ((source & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
branch(std::uint64_t source, std::uint64_t target) noexcept {
  const auto displacement = (static_cast<std::int64_t>(target) -
                             static_cast<std::int64_t>(source)) /
                            4;
  return 0x1400'0000U |
         (static_cast<std::uint32_t>(displacement) & 0x03ff'ffffU);
}

[[nodiscard]] constexpr std::uint32_t
conditional_branch(unsigned condition, std::uint64_t source,
                   std::uint64_t target) noexcept {
  const auto displacement = (static_cast<std::int64_t>(target) -
                             static_cast<std::int64_t>(source)) /
                            4;
  return 0x5400'0000U |
         ((static_cast<std::uint32_t>(displacement) & 0x7ffffU) << 5U) |
         (condition & 15U);
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
  std::size_t executed{};
  const auto event = executor.run_block(cpu, memory, generation, executed);
  return {event, executed};
}

void direct_profile_counts_only_taken_successes() {
  constexpr unsigned long generation = 8701;
  constexpr std::uint64_t base = 0x110'0000;
  constexpr auto target = base + 0x100;
  JitBacking backing{1};
  backing.put32(0, 0xf100'0400U); // subs x0, x0, #1
  backing.put32(4, conditional_branch(1, base + 4U, target)); // b.ne
  backing.put32(8, system_call);
  backing.put32(target - base, add_immediate(2, 2));
  backing.put32(target - base + 4U, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};

  cpu.pc = target;
  require(executor.begin_execution(generation, cpu.pc));
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 2);

  cpu.pc = base;
  cpu.registers[0] = 1;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 2);
  require(cpu.pc == base + 8U);
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 1);
  auto profile = executor.gpr_link_profile_diagnostics();
  require(profile.successful_links == 0);

  cpu.pc = base;
  cpu.registers[0] = 2;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 4);
  profile = executor.gpr_link_profile_diagnostics();
  require(profile.successful_links == 1);
  require(profile.unattributed_links == 0);
  require(profile.dirty_out_saves[0] == 1);
  require(profile.live_in_saves[2] == 1);
  require(profile.full_handoffs[0] == 0 &&
          profile.full_handoffs[2] == 0);
  executor.end_execution();
}

void pic_slots_are_attributed_and_drained_before_replacement_and_reset() {
  constexpr unsigned long generation = 8702;
  constexpr std::uint64_t base = 0x114'0000;
  constexpr auto first = base + 0x100;
  constexpr auto second = base + 0x180;
  constexpr auto third = base + 0x200;
  constexpr auto sentinel = base + 0x300;
  JitBacking backing{1};
  backing.put32(0, add_immediate(4, 4));
  backing.put32(4, branch_register(5));
  backing.put32(first - base, add_immediate(1, 1));
  backing.put32(first - base + 4U, system_call);
  backing.put32(second - base, add_immediate(2, 2));
  backing.put32(second - base + 4U, system_call);
  backing.put32(third - base, add_immediate(3, 3));
  backing.put32(third - base + 4U, system_call);
  backing.put32(sentinel - base, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[5] = first;

  require(executor.begin_execution(generation, cpu.pc));
  auto miss_and_train = [&](std::uint64_t target) {
    cpu.pc = base;
    cpu.registers[5] = target;
    auto run = run_block(executor, memory, generation, cpu);
    require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 2);
    require(cpu.pc == target);
    run = run_block(executor, memory, generation, cpu);
    require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 2);
  };
  const auto hit = [&](std::uint64_t target) {
    cpu.pc = base;
    cpu.registers[5] = target;
    const auto run = run_block(executor, memory, generation, cpu);
    require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 4);
  };

  miss_and_train(first);
  hit(first);
  miss_and_train(second);
  hit(second);
  hit(first);
  miss_and_train(third); // Replaces and drains the first slot.

  auto profile = executor.gpr_link_profile_diagnostics();
  require(profile.successful_links == 3);
  require(profile.unattributed_links == 0);
  require(profile.live_in_saves[1] == 2);
  require(profile.live_in_saves[2] == 1);
  require(profile.live_in_saves[3] == 0);
  require(profile.dirty_out_saves[4] == 3);

  hit(third);
  /*
   * Leave the third-slot counter pending across a code-sequence transition.
   * PIC sites must drain before direct targets lose their mask validity.
   */
  darwin_uml_static_vm_invalidate_code(sentinel);
  cpu.pc = sentinel;
  const auto sentinel_run =
      run_block(executor, memory, generation, cpu);
  require(sentinel_run.event == ::uml::cpu::EngineEvent::system_call);
  profile = executor.gpr_link_profile_diagnostics();
  require(profile.successful_links == 4);
  require(profile.unattributed_links == 0);
  require(profile.live_in_saves[3] == 1);
  require(profile.dirty_out_saves[4] == 4);
  executor.end_execution();
}

void repeated_pic_emissions_keep_their_own_dirty_masks() {
  constexpr unsigned long generation = 8706;
  constexpr std::uint64_t base = 0x124'0000;
  constexpr auto branch_pc = base + 4U;
  constexpr auto target = base + 0x100;
  JitBacking backing{1};
  backing.put32(0, add_immediate(4, 4));
  backing.put32(4, branch_register(5));
  backing.put32(target - base, add_immediate(2, 2));
  backing.put32(target - base + 4U, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.registers[5] = target;

  cpu.pc = target;
  require(executor.begin_execution(generation, cpu.pc));
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);

  cpu.pc = base;
  auto run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::continued && run.executed == 2);
  require(cpu.pc == target);
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call); // Trains the shared site.

  cpu.pc = base;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 4);

  /*
   * Compile the same BR PC as a separate block. Its emitted counter must
   * carry a zero dirty mask rather than inheriting X4 from the earlier
   * two-instruction translation of this site.
   */
  cpu.pc = branch_pc;
  run = run_block(executor, memory, generation, cpu);
  require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 3);
  const auto profile = executor.gpr_link_profile_diagnostics();
  require(profile.successful_links == 2);
  require(profile.unattributed_links == 0);
  require(profile.dirty_out_saves[4] == 1);
  require(profile.live_in_saves[2] == 2);
  executor.end_execution();
}

void fairness_and_publication_rejections_do_not_count() {
  {
    constexpr unsigned long generation = 8707;
    constexpr std::uint64_t base = 0x128'0000;
    JitBacking backing{1};
    backing.put32(0, branch(base, base));
    JitMapping mapping{generation, base, backing, 1};
    JitExecutor executor;
    StaticGuestMemory memory;
    arm64::CpuState cpu{};
    cpu.pc = base;
    require(executor.begin_execution(generation, cpu.pc));
    const auto run = run_block(executor, memory, generation, cpu);
    require(run.event == ::uml::cpu::EngineEvent::continued);
    require(run.executed ==
            uml::cpu::arm64::jit::testing::link_chain_instruction_limit());
    const auto profile = executor.gpr_link_profile_diagnostics();
    require(profile.successful_links + 1U == run.executed);
    require(profile.unattributed_links == 0);
    executor.end_execution();
  }

  {
    constexpr unsigned long generation = 8708;
    constexpr std::uint64_t base = 0x12c'0000;
    constexpr auto target = base + 0x100;
    JitBacking backing{1};
    backing.put32(0, branch(base, target));
    backing.put32(target - base, 0xf900'0020U); // str x0, [x1]
    backing.put32(target - base + 4U, system_call);
    JitMapping mapping{generation, base, backing, 1};
    JitExecutor executor;
    StaticGuestMemory memory;
    arm64::CpuState cpu{};
    cpu.pc = target;
    cpu.registers[1] = 0x1234'0000;
    require(executor.begin_execution(generation, cpu.pc));
    const auto fault = run_block(executor, memory, generation, cpu);
    require(fault.event == ::uml::cpu::EngineEvent::memory_fault);

    cpu.pc = base;
    const auto rejected = run_block(executor, memory, generation, cpu);
    require(rejected.event == ::uml::cpu::EngineEvent::continued && rejected.executed == 1);
    require(cpu.pc == target);
    const auto profile = executor.gpr_link_profile_diagnostics();
    require(profile.successful_links == 0);
    require(profile.unattributed_links == 0);
    executor.end_execution();
  }
}

void process_global_aggregation_merges_executor_deltas_once() {
  constexpr unsigned long generation = 8703;
  constexpr std::uint64_t base = 0x118'0000;
  constexpr auto first_source = base;
  constexpr auto second_source = base + 0x40;
  constexpr auto target = base + 0x100;
  JitBacking backing{1};
  backing.put32(first_source - base, add_immediate(0, 0));
  backing.put32(first_source - base + 4U,
                branch(first_source + 4U, target));
  backing.put32(second_source - base, add_immediate(0, 0));
  backing.put32(second_source - base + 4U,
                branch(second_source + 4U, target));
  backing.put32(target - base, add_immediate(2, 2));
  backing.put32(target - base + 4U, system_call);
  JitMapping mapping{generation, base, backing, 1};
  const auto before =
      uml::cpu::arm64::global_gpr_link_profile_diagnostics();

  {
    JitExecutor first_executor;
    JitExecutor second_executor;
    const auto run_executor = [&](JitExecutor &executor,
                                  std::uint64_t source) {
      StaticGuestMemory memory;
      arm64::CpuState cpu{};
      cpu.pc = target;
      require(executor.begin_execution(generation, cpu.pc));
      auto run = run_block(executor, memory, generation, cpu);
      require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 2);
      cpu.pc = source;
      run = run_block(executor, memory, generation, cpu);
      require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 4);
      const auto local = executor.gpr_link_profile_diagnostics();
      require(local.successful_links == 1);
      require(executor.gpr_link_profile_diagnostics().successful_links == 1);
      executor.end_execution();
    };
    run_executor(first_executor, first_source);
    run_executor(second_executor, second_source);

    const auto before_destruction =
        uml::cpu::arm64::global_gpr_link_profile_diagnostics();
    require(before_destruction.successful_links == before.successful_links);
    require(before_destruction.outer_invocations ==
            before.outer_invocations);
  }

  const auto after =
      uml::cpu::arm64::global_gpr_link_profile_diagnostics();
  require(after.successful_links - before.successful_links == 2);
  require(after.unattributed_links == before.unattributed_links);
  require(after.outer_invocations - before.outer_invocations == 4);
  require(after.dirty_out_saves[0] - before.dirty_out_saves[0] == 2);
  require(after.live_in_saves[2] - before.live_in_saves[2] == 2);
}

void process_global_aggregation_serializes_concurrent_publishers() {
  constexpr std::size_t publishers = 4;
  constexpr std::size_t publications = 250;
  constexpr std::uint32_t dirty = std::uint32_t{1} << 6U;
  constexpr std::uint32_t live_in = std::uint32_t{1} << 7U;
  const auto before =
      uml::cpu::arm64::global_gpr_link_profile_diagnostics();
  std::array<std::thread, publishers> workers;
  for (std::size_t worker = 0; worker < workers.size(); ++worker) {
    workers[worker] = std::thread{[worker] {
      for (std::size_t iteration = 0; iteration < publications; ++iteration)
        uml::cpu::arm64::jit::testing::
            publish_gpr_link_profile_delta_for_testing(
                0x130'0000U + worker * 4U, 0x130'1000U, dirty, live_in, 1,
                false);
    }};
  }
  for (auto &worker : workers)
    worker.join();

  constexpr auto expected = publishers * publications;
  const auto after =
      uml::cpu::arm64::global_gpr_link_profile_diagnostics();
  require(after.successful_links - before.successful_links == expected);
  require(after.outer_invocations - before.outer_invocations == expected);
  require(after.dirty_out_saves[6] - before.dirty_out_saves[6] == expected);
  require(after.live_in_saves[7] - before.live_in_saves[7] == expected);
  require(after.full_handoffs[6] == before.full_handoffs[6]);
  require(after.full_handoffs[7] == before.full_handoffs[7]);
}

void normal_execution_boundaries_preserve_pending_counts() {
  constexpr unsigned long generation = 8709;
  constexpr std::uint64_t base = 0x134'0000;
  constexpr auto target = base + 0x100;
  JitBacking backing{1};
  backing.put32(0, add_immediate(0, 0));
  backing.put32(4, branch(base + 4U, target));
  backing.put32(target - base, add_immediate(2, 2));
  backing.put32(target - base + 4U, system_call);
  JitMapping mapping{generation, base, backing, 1};
  const auto before =
      uml::cpu::arm64::global_gpr_link_profile_diagnostics();

  {
    JitExecutor executor;
    StaticGuestMemory memory;
    arm64::CpuState cpu{};

    cpu.pc = target;
    require(executor.begin_execution(generation, cpu.pc));
    require(run_block(executor, memory, generation, cpu).event ==
            ::uml::cpu::EngineEvent::system_call);
    executor.end_execution();

    for (unsigned iteration = 0; iteration < 2; ++iteration) {
      cpu.pc = base;
      require(executor.begin_execution(generation, cpu.pc));
      const auto linked = run_block(executor, memory, generation, cpu);
      require(linked.event == ::uml::cpu::EngineEvent::system_call && linked.executed == 4);
      executor.end_execution();
    }

    const auto local = executor.gpr_link_profile_diagnostics();
    require(local.successful_links == 2);
    require(local.outer_invocations == 3);
    const auto before_destruction =
        uml::cpu::arm64::global_gpr_link_profile_diagnostics();
    require(before_destruction.successful_links == before.successful_links);
    require(before_destruction.outer_invocations ==
            before.outer_invocations);
  }

  const auto after =
      uml::cpu::arm64::global_gpr_link_profile_diagnostics();
  require(after.successful_links - before.successful_links == 2);
  require(after.outer_invocations - before.outer_invocations == 3);
  require(after.dirty_out_saves[0] - before.dirty_out_saves[0] == 2);
  require(after.live_in_saves[2] - before.live_in_saves[2] == 2);
}

void destructor_publishes_an_unfinished_execution_scope() {
  constexpr unsigned long generation = 8704;
  constexpr std::uint64_t base = 0x11c'0000;
  constexpr auto target = base + 0x100;
  JitBacking backing{1};
  backing.put32(0, add_immediate(0, 0));
  backing.put32(4, branch(base + 4U, target));
  backing.put32(target - base, add_immediate(2, 2));
  backing.put32(target - base + 4U, system_call);
  JitMapping mapping{generation, base, backing, 1};
  const auto before =
      uml::cpu::arm64::global_gpr_link_profile_diagnostics();
  {
    JitExecutor executor;
    StaticGuestMemory memory;
    arm64::CpuState cpu{};
    cpu.pc = target;
    require(executor.begin_execution(generation, cpu.pc));
    require(run_block(executor, memory, generation, cpu).event ==
            ::uml::cpu::EngineEvent::system_call);
    cpu.pc = base;
    require(run_block(executor, memory, generation, cpu).event ==
            ::uml::cpu::EngineEvent::system_call);
    /* Deliberately omit end_execution(); the executor destructor publishes. */
  }
  const auto after =
      uml::cpu::arm64::global_gpr_link_profile_diagnostics();
  require(after.successful_links - before.successful_links == 1);
  require(after.unattributed_links == before.unattributed_links);
}

void arena_reset_recompiles_link_operands_before_installation() {
  constexpr unsigned long generation = 8705;
  constexpr std::uint64_t base = 0x120'0000;
  constexpr auto target =
      base + DARWIN_UML_GUEST_PAGE_SIZE - sizeof(std::uint32_t);
  constexpr std::size_t sources = 512;
  JitBacking backing{1};
  for (std::size_t index = 0; index < sources; ++index) {
    const auto source = base + index * sizeof(std::uint32_t);
    backing.put32(index * sizeof(std::uint32_t), branch(source, target));
  }
  backing.put32(target - base, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor{
      JitExecutorOptions{},
      JitExecutorDiagnosticsOptions{.code_arena_size = 4096}};
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  cpu.pc = target;
  require(executor.begin_execution(generation, cpu.pc));
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);

  std::uint64_t reset_source{};
  std::size_t successful_before_reset{};
  for (std::size_t index = 0; index < sources; ++index) {
    cpu.pc = base + index * sizeof(std::uint32_t);
    const auto run = run_block(executor, memory, generation, cpu);
    if (executor.cache_validation_diagnostics().arena_resets != 0) {
      reset_source = base + index * sizeof(std::uint32_t);
      require(run.event == ::uml::cpu::EngineEvent::continued && cpu.pc == target);
      break;
    }
    require(run.event == ::uml::cpu::EngineEvent::system_call && run.executed == 2);
    ++successful_before_reset;
  }
  require(reset_source != 0);
  require(executor.cache_validation_diagnostics().arena_resets == 1);

  cpu.pc = target;
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);
  cpu.pc = reset_source;
  const auto linked = run_block(executor, memory, generation, cpu);
  require(linked.event == ::uml::cpu::EngineEvent::system_call && linked.executed == 2);
  const auto profile = executor.gpr_link_profile_diagnostics();
  require(profile.successful_links == successful_before_reset + 1U);
  require(profile.unattributed_links == 0);
  executor.end_execution();
}

void explicit_shutdown_hook_writes_one_global_snapshot() {
  constexpr unsigned long generation = 8710;
  constexpr std::uint64_t base = 0x138'0000;
  constexpr auto target = base + 0x100;
  JitBacking backing{1};
  backing.put32(0, add_immediate(0, 0));
  backing.put32(4, branch(base + 4U, target));
  backing.put32(target - base, add_immediate(2, 2));
  backing.put32(target - base + 4U, system_call);
  JitMapping mapping{generation, base, backing, 1};
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::CpuState cpu{};
  const auto before =
      uml::cpu::arm64::global_gpr_link_profile_diagnostics();

  cpu.pc = target;
  require(executor.begin_execution(generation, cpu.pc));
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);
  cpu.pc = base;
  require(run_block(executor, memory, generation, cpu).event ==
          ::uml::cpu::EngineEvent::system_call);
  executor.end_execution();
  require(uml::cpu::arm64::global_gpr_link_profile_diagnostics()
              .successful_links == before.successful_links);

  auto path = uml::testing::scratch_template("uml-jit-gpr-profile.XXXXXX");
  const int descriptor = mkstemp(path.data());
  require(descriptor >= 0);
  require(close(descriptor) == 0);
  require(setenv("UML_JIT_GPR_LINK_PROFILE_PATH", path.c_str(), 1) == 0);
  uml::cpu::arm64::report_global_gpr_link_profile();
  uml::cpu::arm64::report_global_gpr_link_profile();
  const auto after =
      uml::cpu::arm64::global_gpr_link_profile_diagnostics();
  require(after.successful_links - before.successful_links == 1);

  auto *input = std::fopen(path.c_str(), "r");
  require(input != nullptr);
  std::size_t summaries{};
  std::size_t edges{};
  std::size_t registers{};
  std::size_t stack_pointers{};
  std::array<char, 512> line{};
  while (std::fgets(line.data(), static_cast<int>(line.size()), input) !=
         nullptr) {
    summaries +=
        std::strstr(line.data(), "darwin-jit-gpr-link-summary ") != nullptr;
    edges +=
        std::strstr(line.data(), "darwin-jit-gpr-link-edge ") != nullptr;
    registers +=
        std::strstr(line.data(), "darwin-jit-gpr-link-reg ") != nullptr;
    stack_pointers +=
        std::strstr(line.data(), " reg=sp ") != nullptr;
  }
  require(std::fclose(input) == 0);
  require(summaries == 1);
  require(edges != 0);
  require(registers == 32);
  require(stack_pointers == 1);
  require(unsetenv("UML_JIT_GPR_LINK_PROFILE_PATH") == 0);
  require(unlink(path.c_str()) == 0);
}

} // namespace

int main() {
  require(arm64::set_jit_cache_budget(
      256U * 1024U * 1024U, 2));
  require(jit_available() == 1);
  direct_profile_counts_only_taken_successes();
  pic_slots_are_attributed_and_drained_before_replacement_and_reset();
  repeated_pic_emissions_keep_their_own_dirty_masks();
  fairness_and_publication_rejections_do_not_count();
  process_global_aggregation_merges_executor_deltas_once();
  process_global_aggregation_serializes_concurrent_publishers();
  normal_execution_boundaries_preserve_pending_counts();
  destructor_publishes_an_unfinished_execution_scope();
  arena_reset_recompiles_link_operands_before_installation();
  explicit_shutdown_hook_writes_one_global_snapshot();
  return 0;
}
