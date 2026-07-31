// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "runtime/jit/jit_executor.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sys/mman.h>
#include <thread>
#include <vector>

using namespace uml::backend::tests;

namespace {

constexpr std::uint32_t load_exclusive = 0xc85f'7c23U;  // ldxr x3, [x1]
constexpr std::uint32_t alias_store = 0xf900'00c2U;     // str x2, [x6]
constexpr std::uint32_t store_exclusive = 0xc805'7c24U; // stxr w5, x4, [x1]
constexpr std::uint32_t system_call = 0xd400'0001U;     // svc #0

void native_single_block_case() {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr unsigned long generation = 10'099U;
  constexpr std::uint64_t base = 0x10f'0000U;
  constexpr std::uint64_t data = base + page;
  constexpr std::uint32_t add_one = 0x9100'0463U; // add x3, x3, #1
  constexpr std::uint32_t store_loaded =
      0xc805'7c23U; // stxr w5, x3, [x1]
  constexpr std::uint32_t retry = 0x35ff'ffa5U;   // cbnz w5, pc - 12

  JitBacking backing{2};
  backing.put32(0, load_exclusive);
  backing.put32(4, add_one);
  backing.put32(8, store_loaded);
  backing.put32(12, retry);
  backing.put64(page, 41);
  JitMapping mapping{generation, base, backing, 2};

  uml::backend::JitExecutor executor;
  uml::backend::StaticGuestMemory memory;
  uml::backend::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[1] = data;
  require(executor.begin_execution(generation, cpu.pc));
  std::size_t executed = 0;
  require(executor.run_block(cpu, memory, generation, executed) ==
          uml::backend::arm64::gadgets::Event::continued);
  require(executed >= 4);
  require(cpu.registers[5] == 0);
  require(backing.get64(page) == 42);
  executor.end_execution();
}

void native_linked_loop_case() {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr unsigned long generation = 10'098U;
  constexpr std::uint64_t base = 0x10e'0000U;
  constexpr std::uint64_t data = base + page;
  constexpr std::uint32_t add_one = 0x9100'0463U;
  constexpr std::uint32_t store_loaded = 0xc805'7c23U;
  constexpr std::uint32_t retry = 0x35ff'ffa5U;
  constexpr std::uint32_t decrement = 0xf100'0673U;
  constexpr std::uint32_t loop = 0x54ff'ff61U;

  JitBacking backing{2};
  backing.put32(0, load_exclusive);
  backing.put32(4, add_one);
  backing.put32(8, store_loaded);
  backing.put32(12, retry);
  backing.put32(16, decrement);
  backing.put32(20, loop);
  backing.put32(24, system_call);
  backing.put64(page, 0);
  JitMapping mapping{generation, base, backing, 2};

  uml::backend::JitExecutor executor;
  uml::backend::StaticGuestMemory memory;
  uml::backend::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[1] = data;
  cpu.registers[19] = 500;
  require(executor.begin_execution(generation, cpu.pc));
  std::size_t executed = 0;
  auto event = uml::backend::arm64::gadgets::Event::continued;
  for (unsigned attempt = 0;
       attempt < 10U &&
       event == uml::backend::arm64::gadgets::Event::continued;
       ++attempt)
    event = executor.run_block(cpu, memory, generation, executed);
  require(event == uml::backend::arm64::gadgets::Event::system_call);
  require(cpu.registers[19] == 0);
  require(backing.get64(page) == 500);
  executor.end_execution();
}

void native_compare_retry_case(bool matches) {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const auto generation =
      static_cast<unsigned long>(matches ? 10'096U : 10'097U);
  const auto base =
      std::uint64_t{0x10c'0000U} + (matches ? 0U : 4U * page);
  const auto data = base + page;
  constexpr std::uint32_t compare = 0xeb04'007fU; // cmp x3, x4
  constexpr std::uint32_t skip = 0x5400'0061U;    // b.ne pc + 12
  constexpr std::uint32_t store =
      0xc805'7c22U; // stxr w5, x2, [x1]
  constexpr std::uint32_t retry = 0x35ff'ff85U; // cbnz w5, pc - 16
  constexpr std::uint64_t initial = 41;
  constexpr std::uint64_t replacement = 99;

  JitBacking backing{2};
  backing.put32(0, load_exclusive);
  backing.put32(4, compare);
  backing.put32(8, skip);
  backing.put32(12, store);
  backing.put32(16, retry);
  backing.put32(20, system_call);
  backing.put64(page, initial);
  JitMapping mapping{generation, base, backing, 2};

  uml::backend::JitExecutor executor;
  uml::backend::StaticGuestMemory memory;
  uml::backend::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[1] = data;
  cpu.registers[2] = replacement;
  cpu.registers[4] = matches ? initial : initial + 1U;
  cpu.registers[5] = 7;
  require(executor.begin_execution(generation, cpu.pc));
  std::size_t total_executed = 0;
  auto event = uml::backend::arm64::gadgets::Event::continued;
  for (std::size_t attempt = 0;
       attempt < 10U &&
       event == uml::backend::arm64::gadgets::Event::continued;
       ++attempt) {
    std::size_t executed = 0;
    event = executor.run_block(cpu, memory, generation, executed);
    total_executed += executed;
  }
  require(event == uml::backend::arm64::gadgets::Event::system_call);
  require(total_executed == (matches ? 6U : 4U));
  require(cpu.registers[3] == initial);
  require(cpu.registers[5] == (matches ? 0U : 7U));
  require(backing.get64(page) == (matches ? replacement : initial));
  require((cpu.nzcv & 0x4000'0000U) ==
          (matches ? 0x4000'0000U : 0U));
  executor.end_execution();
}

void same_backing_alias_case(bool overlaps) {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const auto generation =
      static_cast<unsigned long>(overlaps ? 10'100U : 10'101U);
  const auto base =
      std::uint64_t{0x110'0000} + (overlaps ? 0U : 8U * page);
  const auto data = base + page;
  const auto alias = base + 4U * page;
  const auto store_offset = overlaps ? std::size_t{0} : std::size_t{32};
  constexpr std::uint64_t observed = 0x1122'3344'5566'7788ULL;
  constexpr std::uint64_t replacement = 0x8877'6655'4433'2211ULL;

  JitBacking backing{2};
  backing.put32(0, load_exclusive);
  backing.put32(4, system_call);
  backing.put32(8, alias_store);
  backing.put32(12, system_call);
  backing.put32(16, store_exclusive);
  backing.put32(20, system_call);
  backing.put64(page, observed);
  backing.put64(page + store_offset, observed);
  JitMapping mapping{generation, base, backing, 2};
  require(darwin_uml_static_vm_map(
              reinterpret_cast<void *>(alias), backing.descriptor(), page,
              page, PROT_READ | PROT_WRITE, 0, 1) == 0);

  uml::backend::JitExecutor first;
  uml::backend::JitExecutor second;
  uml::backend::StaticGuestMemory shared_memory;
  uml::backend::arm64::CpuState first_cpu{};
  uml::backend::arm64::CpuState second_cpu{};
  first_cpu.pc = base;
  first_cpu.registers[1] = data;
  first_cpu.registers[4] = replacement;
  /*
   * Exercise the checked semantic reservation path deliberately. Untagged
   * production blocks lower base exclusive instructions directly to the host
   * and may legitimately lose their monitor across this artificial sequence
   * of separate run_block calls.
   */
  first_cpu.mte_enabled = true;
  second_cpu.pc = base + 8U;
  second_cpu.registers[2] = observed;
  second_cpu.registers[6] = alias + store_offset;

  require(first.begin_execution(generation, first_cpu.pc));
  std::size_t executed = 0;
  require(first.run_block(first_cpu, shared_memory, generation, executed) ==
          uml::backend::arm64::gadgets::Event::continued);
  require(executed == 1);
  require(first_cpu.exclusive_size == 8);
  require(first_cpu.registers[3] == observed);

  require(second.begin_execution(generation, second_cpu.pc));
  require(second.run_block(second_cpu, shared_memory, generation, executed) ==
          uml::backend::arm64::gadgets::Event::system_call);
  require(executed == 2);
  second.end_execution();

  first_cpu.pc = base + 16U;
  require(first.run_block(first_cpu, shared_memory, generation, executed) ==
          uml::backend::arm64::gadgets::Event::continued);
  require(executed == 1);
  require(first_cpu.registers[5] == (overlaps ? 1U : 0U));
  require(first_cpu.exclusive_size == 0);
  require(backing.get64(page) == (overlaps ? observed : replacement));
  require(backing.get64(page + store_offset) == observed);
  first.end_execution();
}

void native_store_invalidates_semantic_reservation_case() {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr unsigned long generation = 10'102U;
  constexpr std::uint64_t base = 0x118'0000U;
  constexpr std::uint64_t data = base + page;
  constexpr std::uint32_t add_one = 0x9100'0463U;
  constexpr std::uint32_t native_store = 0xc805'7c23U;
  constexpr std::uint64_t initial = 41U;

  JitBacking backing{2};
  backing.put32(0, load_exclusive);
  backing.put32(4, system_call);
  backing.put32(8, load_exclusive);
  backing.put32(12, add_one);
  backing.put32(16, native_store);
  backing.put32(20, system_call);
  backing.put32(24, store_exclusive);
  backing.put32(28, system_call);
  backing.put64(page, initial);
  JitMapping mapping{generation, base, backing, 2};

  uml::backend::JitExecutor semantic_executor;
  uml::backend::JitExecutor native_executor;
  uml::backend::StaticGuestMemory memory;
  uml::backend::arm64::CpuState semantic_cpu{};
  uml::backend::arm64::CpuState native_cpu{};
  semantic_cpu.pc = base;
  semantic_cpu.registers[1] = data;
  semantic_cpu.registers[4] = 99U;
  semantic_cpu.mte_enabled = true;
  native_cpu.pc = base + 8U;
  native_cpu.registers[1] = data;

  require(semantic_executor.begin_execution(generation, semantic_cpu.pc));
  std::size_t executed = 0;
  require(semantic_executor.run_block(
              semantic_cpu, memory, generation, executed) ==
          uml::backend::arm64::gadgets::Event::continued);
  require(executed == 1U);
  require(semantic_cpu.exclusive_size == 8U);

  require(native_executor.begin_execution(generation, native_cpu.pc));
  auto native_event = uml::backend::arm64::gadgets::Event::continued;
  for (unsigned attempt = 0;
       attempt < 4U &&
       native_event == uml::backend::arm64::gadgets::Event::continued;
       ++attempt)
    native_event = native_executor.run_block(
        native_cpu, memory, generation, executed);
  require(native_event ==
          uml::backend::arm64::gadgets::Event::system_call);
  require(native_cpu.registers[5] == 0U);
  require(backing.get64(page) == initial + 1U);
  native_executor.end_execution();

  semantic_cpu.pc = base + 24U;
  require(semantic_executor.run_block(
              semantic_cpu, memory, generation, executed) ==
          uml::backend::arm64::gadgets::Event::continued);
  require(executed == 1U);
  require(semantic_cpu.registers[5] == 1U);
  require(semantic_cpu.exclusive_size == 0U);
  require(backing.get64(page) == initial + 1U);
  semantic_executor.end_execution();
}

void concurrent_native_cold_paths_leave_publication_before_helper() {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr unsigned long generation = 10'103U;
  constexpr std::uint64_t base = 0x11c'0000U;
  constexpr std::uint64_t data = base + page;
  constexpr std::size_t workers = 10U;
  constexpr std::uint32_t add_one = 0x9100'0463U;
  constexpr std::uint32_t store_loaded = 0xc805'7c23U;
  constexpr std::uint32_t retry = 0x35ff'ffa5U;

  JitBacking backing{2};
  backing.put32(0, load_exclusive);
  backing.put32(4, add_one);
  backing.put32(8, store_loaded);
  backing.put32(12, retry);
  for (std::size_t index = 0; index < workers; ++index)
    backing.put64(page + index * sizeof(std::uint64_t), index);
  JitMapping mapping{generation, base, backing, 2};

  auto executors =
      std::make_unique<std::array<uml::backend::JitExecutor, workers>>();
  auto memories =
      std::make_unique<std::array<uml::backend::StaticGuestMemory, workers>>();
  auto cpus = std::make_unique<
      std::array<uml::backend::arm64::CpuState, workers>>();
  /*
   * Compile and warm each native sequence before the synchronized run. The
   * tagged-state switch then forces every cached sequence down the helper
   * cold edge without allowing translation latency to serialize the test.
   */
  for (std::size_t index = 0; index < workers; ++index) {
    auto &cpu = (*cpus)[index];
    cpu.pc = base;
    cpu.registers[1] = data + index * sizeof(std::uint64_t);
    require((*executors)[index].begin_execution(generation, cpu.pc));
    std::size_t executed = 0;
    require((*executors)[index].run_block(
                cpu, (*memories)[index], generation, executed) ==
            uml::backend::arm64::gadgets::Event::continued);
    require(executed >= 4U);
    (*executors)[index].end_execution();
    cpu.pc = base;
    cpu.mte_enabled = true;
  }

  std::barrier start{workers};
  std::atomic<bool> valid{true};
  std::vector<std::thread> threads;
  threads.reserve(workers);
  for (std::size_t index = 0; index < workers; ++index) {
    threads.emplace_back([&, index] {
      auto &cpu = (*cpus)[index];
      auto &executor = (*executors)[index];
      if (!executor.begin_execution(generation, cpu.pc)) {
        valid.store(false, std::memory_order_relaxed);
        start.arrive_and_drop();
        return;
      }
      start.arrive_and_wait();
      std::size_t executed = 0;
      const auto event =
          executor.run_block(
              cpu, (*memories)[index], generation, executed);
      if (event != uml::backend::arm64::gadgets::Event::continued ||
          executed != 1U || cpu.pc != base + sizeof(std::uint32_t))
        valid.store(false, std::memory_order_relaxed);
      executor.end_execution();
    });
  }
  for (auto &thread : threads)
    thread.join();
  require(valid.load(std::memory_order_relaxed));
}

} // namespace

int main() {
  native_single_block_case();
  native_linked_loop_case();
  native_compare_retry_case(true);
  native_compare_retry_case(false);
  same_backing_alias_case(true);
  same_backing_alias_case(false);
  native_store_invalidates_semantic_reservation_case();
  concurrent_native_cold_paths_leave_publication_before_helper();
  return 0;
}
