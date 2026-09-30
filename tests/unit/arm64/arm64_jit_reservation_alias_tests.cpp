// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_semantic_memory.hpp"
#include "platform/apple/host_features.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sys/mman.h>
#include <thread>
#include <vector>

using namespace uml::emulator::tests;

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

  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[1] = data;
  require(executor.begin_execution(generation, cpu.pc));
  std::size_t executed = 0;
  require(executor.run_block(cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
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

  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[1] = data;
  cpu.registers[19] = 500;
  require(executor.begin_execution(generation, cpu.pc));
  std::size_t executed = 0;
  auto event = ::uml::cpu::EngineEvent::continued;
  for (unsigned attempt = 0;
       attempt < 10U &&
       event == ::uml::cpu::EngineEvent::continued;
       ++attempt)
    event = executor.run_block(cpu, memory, generation, executed);
  require(event == ::uml::cpu::EngineEvent::system_call);
  require(cpu.registers[19] == 0);
  require(backing.get64(page) == 500);
  executor.end_execution();
}

void native_compare_retry_case(unsigned size, bool acquire_release,
                               bool matches, bool read_only = false) {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const auto generation =
      static_cast<unsigned long>(read_only ? 10'106U :
                                matches ? 10'096U : 10'097U) +
      32U * size + (acquire_release ? 16U : 0U);
  const auto base =
      std::uint64_t{0x10c'0000U} + (matches ? 0U : 4U * page);
  const auto data = base + page;
  const std::uint32_t compare =
      size == 3U ? 0xeb04'007fU : 0x6b04'007fU; // cmp x3/w3, x4/w4
  constexpr std::uint32_t skip = 0x5400'0061U;    // b.ne pc + 12
  const auto order = acquire_release ? 0x8000U : 0U;
  const auto load = 0x085f'7c23U | (size << 30U) | order;
  const auto store = 0x0805'7c22U | (size << 30U) | order;
  constexpr std::uint32_t retry = 0x35ff'ff85U; // cbnz w5, pc - 16
  constexpr std::uint64_t initial = 0x1020'3040'5060'7011ULL;
  constexpr std::uint64_t replacement = 0x8877'6655'4433'2222ULL;
  const auto mask = size == 3U ? ~std::uint64_t{} :
      (std::uint64_t{1} << (8U << size)) - 1U;
  const auto loaded = initial & mask;

  JitBacking backing{2};
  backing.put32(0, load);
  backing.put32(4, compare);
  backing.put32(8, skip);
  backing.put32(12, store);
  backing.put32(16, retry);
  backing.put32(20, system_call);
  backing.put64(page, initial);
  JitMapping mapping{generation, base, backing, 2};
  if (read_only)
    require(darwin_uml_static_vm_protect(
        reinterpret_cast<void *>(data), page, PROT_READ) == 0);

  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[1] = data;
  cpu.registers[2] = replacement;
  cpu.registers[4] = matches ? loaded : loaded + 1U;
  cpu.registers[5] = 7;
  require(executor.begin_execution(generation, cpu.pc));
  std::size_t total_executed = 0;
  auto event = ::uml::cpu::EngineEvent::continued;
  for (std::size_t attempt = 0;
       attempt < 10U &&
       event == ::uml::cpu::EngineEvent::continued;
       ++attempt) {
    std::size_t executed = 0;
    event = executor.run_block(cpu, memory, generation, executed);
    total_executed += executed;
  }
  require(event == ::uml::cpu::EngineEvent::system_call);
  if (matches) {
    // A host STXR may fail spuriously, including on its first writable-page
    // fault. Each complete retry still retires exactly five instructions.
    require(total_executed >= 6U && (total_executed - 6U) % 5U == 0U);
  } else {
    require(total_executed == 4U);
  }
  require(cpu.registers[3] == loaded);
  require(cpu.registers[5] == (matches ? 0U : 7U));
  require(backing.get64(page) ==
          (matches ? (initial & ~mask) | (replacement & mask) : initial));
  require(cpu.nzcv == (matches ? 0x6000'0000U : 0x8000'0000U));
  executor.end_execution();
}

void compare_lookahead_preserves_fetch_faults(bool matches) {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const unsigned long generation = matches ? 10'108U : 10'107U;
  constexpr std::uint64_t base = 0x130'0000U;
  JitBacking backing{1};
  backing.put64(0, 41);
  backing.put32(page - 64U, system_call);
  backing.put32(page - 12U, load_exclusive);
  backing.put32(page - 8U, 0xeb04'007fU); // cmp x3, x4
  backing.put32(page - 4U, 0x54ff'fe21U); // b.ne pc - 60
  JitMapping mapping{generation, base, backing, 1};
  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  cpu.pc = base + page - 12U;
  cpu.registers[1] = base;
  cpu.registers[4] = matches ? 41U : 42U;
  require(executor.begin_execution(generation, cpu.pc));
  auto event = ::uml::cpu::EngineEvent::continued;
  std::size_t retired = 0;
  for (unsigned attempt = 0; attempt < 10U &&
       event == ::uml::cpu::EngineEvent::continued; ++attempt) {
    std::size_t executed = 0;
    event = executor.run_block(cpu, memory, generation, executed);
    retired += executed;
  }
  require(cpu.registers[3] == 41U);
  if (matches) {
    require(event == ::uml::cpu::EngineEvent::memory_fault);
    require(retired == 3U && cpu.pc == base + page);
    require(executor.fault_address() == base + page);
    require(executor.fault_access() == uml::memory::Access::execute);
  } else {
    require(event == ::uml::cpu::EngineEvent::system_call);
    require(retired == 4U);
  }
  executor.end_execution();
}

void concurrent_compare_retry_case(bool mixed_semantic) {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const unsigned long generation = mixed_semantic ? 10'105U : 10'104U;
  constexpr std::uint64_t base = 0x124'0000U;
  constexpr auto data = base + page;
  constexpr std::size_t workers = 4;
  constexpr unsigned iterations = 1000;
  JitBacking backing{2};
  backing.put32(0, load_exclusive);
  backing.put32(4, 0xeb04'007fU); // cmp x3, x4
  backing.put32(8, 0x5400'0061U); // b.ne end
  backing.put32(12, 0xc805'7c22U); // stxr w5, x2, [x1]
  backing.put32(16, 0x35ff'ff85U); // cbnz w5, start
  backing.put32(20, system_call);
  backing.put64(page, 0);
  JitMapping mapping{generation, base, backing, 2};
  auto executors =
      std::make_unique<std::array<uml::cpu::arm64::JitExecutor, workers>>();
  auto memories =
      std::make_unique<std::array<uml::memory::StaticGuestMemory, workers>>();
  auto cpus = std::make_unique<
      std::array<uml::cpu::arm64::CpuState, workers>>();
  std::barrier start{workers};
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < workers; ++index) {
    threads.emplace_back([&, index] {
      auto &cpu = (*cpus)[index];
      auto &executor = (*executors)[index];
      cpu.registers[1] = data;
      cpu.tbi_enabled = mixed_semantic && index == 0;
      require(executor.begin_execution(generation, base));
      start.arrive_and_wait();
      std::uint64_t expected = 0;
      unsigned successes = 0;
      while (successes < iterations) {
        cpu.pc = base;
        cpu.registers[2] = expected + 1;
        cpu.registers[4] = expected;
        cpu.registers[5] = 7;
        auto event = ::uml::cpu::EngineEvent::continued;
        std::size_t retired = 0;
        for (unsigned attempt = 0; attempt < 10000 &&
             event == ::uml::cpu::EngineEvent::continued; ++attempt) {
          std::size_t executed = 0;
          event = executor.run_block(cpu, (*memories)[index],
                                     generation, executed);
          retired += executed;
        }
        require(event == ::uml::cpu::EngineEvent::system_call);
        if (cpu.registers[5] == 0) {
          require(cpu.registers[3] == expected);
          require((cpu.nzcv & 0x4000'0000U) != 0);
          require(retired >= 6 && (retired - 6) % 5 == 0);
          ++successes;
          expected = cpu.registers[2];
        } else {
          // A mismatch after a failed STXR retains its status of one;
          // a mismatch on the first load retains the sentinel of seven.
          // Both outcomes also occur in the same loop on native ARM64.
          require(cpu.registers[5] == 1 || cpu.registers[5] == 7);
          require(cpu.registers[3] > expected);
          require((cpu.nzcv & 0x4000'0000U) == 0);
          require(retired >= 4 && (retired - 4) % 5 == 0);
          expected = cpu.registers[3];
        }
      }
      executor.end_execution();
    });
  }
  for (auto &thread : threads)
    thread.join();
  require(backing.get64(page) == workers * iterations);
}

enum class AliasStore : unsigned { plain, release, limited_release };

void same_backing_alias_case(bool overlaps, bool semantic_store = false,
                             AliasStore store = AliasStore::plain) {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const auto generation =
      static_cast<unsigned long>((overlaps ? 10'100U : 10'101U) +
                                 (semantic_store ? 20U : 0U) +
                                 40U * static_cast<unsigned>(store));
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

  uml::cpu::arm64::JitExecutor first;
  uml::cpu::arm64::JitExecutor second;
  uml::memory::StaticGuestMemory shared_memory;
  uml::cpu::arm64::CpuState first_cpu{};
  uml::cpu::arm64::CpuState second_cpu{};
  first_cpu.pc = base;
  first_cpu.registers[1] = data;
  first_cpu.registers[4] = replacement;
  /*
   * Exercise the checked semantic reservation path deliberately. Untagged
   * production blocks lower base exclusive instructions directly to the host
   * and may legitimately lose their monitor across this artificial sequence
   * of separate run_block calls.
   */
  first_cpu.tbi_enabled = true;
  second_cpu.pc = base + 8U;
  second_cpu.registers[2] = observed;
  second_cpu.registers[6] = alias + store_offset;
  /*
   * Top-byte-ignore keeps tag checks off but sends the alias store through
   * the JIT's semantic slow path instead of its tracked native store.
   */
  second_cpu.tbi_enabled = semantic_store;

  require(first.begin_execution(generation, first_cpu.pc));
  std::size_t executed = 0;
  require(first.run_block(first_cpu, shared_memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(executed == 1);
  require(first_cpu.exclusive_size == 8);
  require(first_cpu.registers[3] == observed);

  if (store == AliasStore::plain) {
    require(second.begin_execution(generation, second_cpu.pc));
    auto event = ::uml::cpu::EngineEvent::continued;
    std::size_t retired = 0;
    unsigned blocks = 0;
    for (; blocks < 4U && event == ::uml::cpu::EngineEvent::continued;
         ++blocks) {
      event = second.run_block(second_cpu, shared_memory, generation,
                               executed);
      retired += executed;
    }
    require(event == ::uml::cpu::EngineEvent::system_call);
    require(retired == 2);
    // The native store and system call share one translated block.
    require(semantic_store || blocks == 1);
    second.end_execution();
  } else {
    // Exercise the helper explicitly so native lowering cannot hide a stale
    // software reservation on an ordered instruction's cold path.
    uml::memory::Tlb tlb;
    tlb.prepare(generation, darwin_uml_static_vm_changes(generation));
    const auto instruction = store == AliasStore::release ?
        0xc89f'fcc2U : 0xc89f'7cc2U; // stlr/stllr x2, [x6]
    const auto result = uml::cpu::arm64::uml_arm64_jit_execute_ordered(
        &second_cpu, &tlb, instruction);
    require((result >> 32U) == 1U);
    require(static_cast<std::uint32_t>(result) ==
            static_cast<std::uint32_t>(::uml::cpu::EngineEvent::continued));
    require(second_cpu.pc == base + 12U);
  }

  first_cpu.pc = base + 16U;
  require(first.run_block(first_cpu, shared_memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
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

  uml::cpu::arm64::JitExecutor semantic_executor;
  uml::cpu::arm64::JitExecutor native_executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState semantic_cpu{};
  uml::cpu::arm64::CpuState native_cpu{};
  semantic_cpu.pc = base;
  semantic_cpu.registers[1] = data;
  semantic_cpu.registers[4] = 99U;
  semantic_cpu.tbi_enabled = true;
  native_cpu.pc = base + 8U;
  native_cpu.registers[1] = data;

  require(semantic_executor.begin_execution(generation, semantic_cpu.pc));
  std::size_t executed = 0;
  require(semantic_executor.run_block(
              semantic_cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(executed == 1U);
  require(semantic_cpu.exclusive_size == 8U);

  require(native_executor.begin_execution(generation, native_cpu.pc));
  auto native_event = ::uml::cpu::EngineEvent::continued;
  for (unsigned attempt = 0;
       attempt < 4U &&
       native_event == ::uml::cpu::EngineEvent::continued;
       ++attempt)
    native_event = native_executor.run_block(
        native_cpu, memory, generation, executed);
  require(native_event ==
          ::uml::cpu::EngineEvent::system_call);
  require(native_cpu.registers[5] == 0U);
  require(backing.get64(page) == initial + 1U);
  native_executor.end_execution();

  semantic_cpu.pc = base + 24U;
  require(semantic_executor.run_block(
              semantic_cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(executed == 1U);
  require(semantic_cpu.registers[5] == 1U);
  require(semantic_cpu.exclusive_size == 0U);
  require(backing.get64(page) == initial + 1U);
  semantic_executor.end_execution();
}

/*
 * A pair or structure store that the JIT leaves to its slow helper, here
 * because it crosses a page, must invalidate another vCPU's software
 * reservation on the bytes it writes even when it stores the value already
 * there. Otherwise that vCPU's STXR succeeds over the intervening store.
 */
void slow_store_invalidates_semantic_reservation_case(bool structure) {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const unsigned long generation = structure ? 10'123U : 10'122U;
  const std::uint64_t base = structure ? 0x158'0000U : 0x150'0000U;
  const auto data = base + 2U * page;
  constexpr std::uint64_t observed = 0x1122'3344'5566'7788ULL;
  constexpr std::uint64_t replacement = 0x8877'6655'4433'2211ULL;
  constexpr std::uint32_t crossing_pair = 0xa900'08c2U; // stp x2, x2, [x6]
  constexpr std::uint32_t crossing_structure =
      0x4c00'70c0U; // st1 {v0.16b}, [x6]

  JitBacking backing{3};
  backing.put32(0, load_exclusive);
  backing.put32(4, system_call);
  backing.put32(8, structure ? crossing_structure : crossing_pair);
  backing.put32(12, system_call);
  backing.put32(16, store_exclusive);
  backing.put32(20, system_call);
  backing.put64(2U * page - 8U, observed);
  backing.put64(2U * page, observed);
  JitMapping mapping{generation, base, backing, 3};

  uml::cpu::arm64::JitExecutor semantic_executor;
  uml::cpu::arm64::JitExecutor store_executor;
  uml::memory::StaticGuestMemory memory;
  auto cpus = std::make_unique<
      std::array<uml::cpu::arm64::CpuState, 2>>();
  auto &semantic_cpu = (*cpus)[0];
  auto &store_cpu = (*cpus)[1];
  semantic_cpu.pc = base;
  semantic_cpu.registers[1] = data;
  semantic_cpu.registers[4] = replacement;
  semantic_cpu.tbi_enabled = true;
  store_cpu.pc = base + 8U;
  store_cpu.registers[2] = observed;
  store_cpu.registers[6] = data - 8U;
  std::memcpy(store_cpu.vectors[0].bytes.data(), &observed, 8U);
  std::memcpy(store_cpu.vectors[0].bytes.data() + 8U, &observed, 8U);

  require(semantic_executor.begin_execution(generation, semantic_cpu.pc));
  std::size_t executed = 0;
  require(semantic_executor.run_block(
              semantic_cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(executed == 1U);
  require(semantic_cpu.exclusive_size == 8U);

  require(store_executor.begin_execution(generation, store_cpu.pc));
  auto store_event = ::uml::cpu::EngineEvent::continued;
  std::size_t retired = 0;
  for (unsigned attempt = 0;
       attempt < 4U &&
       store_event == ::uml::cpu::EngineEvent::continued;
       ++attempt) {
    store_event = store_executor.run_block(
        store_cpu, memory, generation, executed);
    retired += executed;
  }
  require(store_event == ::uml::cpu::EngineEvent::system_call);
  require(retired == 2U);
  require(backing.get64(2U * page - 8U) == observed);
  require(backing.get64(2U * page) == observed);
  store_executor.end_execution();

  semantic_cpu.pc = base + 16U;
  require(semantic_executor.run_block(
              semantic_cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(executed == 1U);
  require(semantic_cpu.registers[5] == 1U);
  require(semantic_cpu.exclusive_size == 0U);
  require(backing.get64(2U * page) == observed);
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
      std::make_unique<std::array<uml::cpu::arm64::JitExecutor, workers>>();
  auto memories =
      std::make_unique<std::array<uml::memory::StaticGuestMemory, workers>>();
  auto cpus = std::make_unique<
      std::array<uml::cpu::arm64::CpuState, workers>>();
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
            ::uml::cpu::EngineEvent::continued);
    require(executed >= 4U);
    (*executors)[index].end_execution();
    cpu.pc = base;
    cpu.tbi_enabled = true;
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
      if (event != ::uml::cpu::EngineEvent::continued ||
          executed != 1U || cpu.pc != base + sizeof(std::uint32_t))
        valid.store(false, std::memory_order_relaxed);
      executor.end_execution();
    });
  }
  for (auto &thread : threads)
    thread.join();
  require(valid.load(std::memory_order_relaxed));
}

/*
 * A compare-and-swap the JIT leaves to its semantic helper must be one host
 * atomic. vCPU A runs a helper-routed compare-and-swap loop that increments
 * one field; vCPU B runs a native LDADD loop on bytes the swap also covers,
 * concurrently. A read-compare-write that is not atomic writes back B's old
 * bytes and loses its increments; each field must end at its own count.
 */
struct HelperSwapRace final {
  unsigned long generation;
  std::uint64_t base;
  std::array<std::uint32_t, 12> swap_loop;
  std::size_t swap_words;
  std::size_t swap_offset;
  std::array<std::uint32_t, 4> add_loop;
  std::size_t add_offset;
  std::uint64_t swap_mask;
  unsigned add_shift;
  std::uint64_t add_mask;
};

void helper_swap_keeps_native_updates(const HelperSwapRace &race) {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const auto data = race.base + page;
  constexpr std::uint64_t add_code = 0x100U;
  constexpr unsigned swaps = 20000;
  constexpr std::uint64_t unbounded = std::uint64_t{1} << 40;

  JitBacking backing{2};
  for (std::size_t index = 0; index < race.swap_words; ++index)
    backing.put32(index * 4U, race.swap_loop[index]);
  for (std::size_t index = 0; index < race.add_loop.size(); ++index)
    backing.put32(add_code + index * 4U, race.add_loop[index]);
  JitMapping mapping{race.generation, race.base, backing, 2};

  uml::cpu::arm64::JitExecutor swap_executor;
  uml::cpu::arm64::JitExecutor add_executor;
  uml::memory::StaticGuestMemory swap_memory;
  uml::memory::StaticGuestMemory add_memory;
  auto cpus = std::make_unique<
      std::array<uml::cpu::arm64::CpuState, 2>>();
  auto &swap_cpu = (*cpus)[0];
  auto &add_cpu = (*cpus)[1];
  swap_cpu.pc = race.base;
  swap_cpu.registers[1] = data + race.swap_offset;
  swap_cpu.registers[19] = swaps;
  add_cpu.pc = race.base + add_code;
  add_cpu.registers[8] = data + race.add_offset;
  add_cpu.registers[9] = 1;
  add_cpu.registers[19] = unbounded;

  std::atomic<bool> swapping{true};
  std::atomic<bool> valid{true};
  std::barrier start{2};
  std::thread adder{[&] {
    if (!add_executor.begin_execution(race.generation, add_cpu.pc))
      valid.store(false, std::memory_order_relaxed);
    start.arrive_and_wait();
    std::size_t executed = 0;
    while (swapping.load(std::memory_order_acquire) &&
           valid.load(std::memory_order_relaxed)) {
      if (add_executor.run_block(add_cpu, add_memory, race.generation,
                                 executed) !=
          ::uml::cpu::EngineEvent::continued)
        valid.store(false, std::memory_order_relaxed);
    }
    add_executor.end_execution();
  }};
  if (!swap_executor.begin_execution(race.generation, swap_cpu.pc))
    valid.store(false, std::memory_order_relaxed);
  start.arrive_and_wait();
  auto event = ::uml::cpu::EngineEvent::continued;
  std::size_t executed = 0;
  for (std::uint64_t attempt = 0;
       attempt < 100'000'000U &&
       event == ::uml::cpu::EngineEvent::continued;
       ++attempt)
    event = swap_executor.run_block(swap_cpu, swap_memory,
                                    race.generation, executed);
  swapping.store(false, std::memory_order_release);
  adder.join();
  swap_executor.end_execution();
  require(valid.load(std::memory_order_relaxed));
  require(event == ::uml::cpu::EngineEvent::system_call);
  require(swap_cpu.registers[19] == 0);

  const auto adds = unbounded - add_cpu.registers[19];
  const auto low = backing.get64(page);
  const auto high = backing.get64(page + 8U);
  const unsigned __int128 memory =
      (static_cast<unsigned __int128>(high) << 64U) | low;
  const auto swapped =
      static_cast<std::uint64_t>(memory >> (race.swap_offset * 8U)) &
      race.swap_mask;
  const auto added =
      static_cast<std::uint64_t>(memory >> race.add_shift) & race.add_mask;
  require(adds != 0);
  require(swapped == (swaps & race.swap_mask));
  require(added == (adds & race.add_mask));
}

void helper_pair_compare_swap_keeps_native_updates() {
  /* CASP always takes the helper. A increments the low doubleword with
   * CASP; B's native LDADD increments the high one. */
  helper_swap_keeps_native_updates({
      .generation = 10'120U,
      .base = 0x140'0000U,
      .swap_loop = {0xa940'0c22U,   // ldp x2, x3, [x1]
                    0x9100'0444U,   // add x4, x2, #1
                    0xaa03'03e5U,   // mov x5, x3
                    0xaa02'03e6U,   // mov x6, x2
                    0xaa03'03e7U,   // mov x7, x3
                    0x4822'7c24U,   // casp x2, x3, x4, x5, [x1]
                    0xeb06'005fU,   // cmp x2, x6
                    0xfa47'0060U,   // ccmp x3, x7, #0, eq
                    0x54ff'ff01U,   // b.ne start
                    0xf100'0673U,   // subs x19, x19, #1
                    0x54ff'fec1U,   // b.ne start
                    system_call},
      .swap_words = 12,
      .swap_offset = 0,
      .add_loop = {0xf829'010aU,    // ldadd x9, x10, [x8]
                   0xf100'0673U,    // subs x19, x19, #1
                   0x54ff'ffc1U,    // b.ne start
                   system_call},
      .add_offset = 8,
      .swap_mask = ~std::uint64_t{},
      .add_shift = 64,
      .add_mask = ~std::uint64_t{},
  });
}

void helper_misaligned_compare_swap_keeps_native_updates() {
  /* With FEAT_LSE2 a CAS misaligned inside one 16-byte block completes, and
   * the JIT leaves it to the helper. A's word covers bytes 2-5; B's native
   * LDADDH increments bytes 4-5, the upper half of that word. A's own
   * increments stay in bytes 2-3 because it swaps fewer than 65536 times. */
  if (!::uml::platform::apple::probed_host_capabilities().has(
          ::uml::cpu::ArmFeature::lse2))
    return;
  helper_swap_keeps_native_updates({
      .generation = 10'121U,
      .base = 0x148'0000U,
      .swap_loop = {0xb940'0022U,   // ldr w2, [x1]
                    0x1100'0444U,   // add w4, w2, #1
                    0x2a02'03e6U,   // mov w6, w2
                    0x88a2'7c24U,   // cas w2, w4, [x1]
                    0x6b06'005fU,   // cmp w2, w6
                    0x54ff'ff61U,   // b.ne start
                    0xf100'0673U,   // subs x19, x19, #1
                    0x54ff'ff21U,   // b.ne start
                    system_call},
      .swap_words = 9,
      .swap_offset = 2,
      .add_loop = {0x7829'010aU,    // ldaddh w9, w10, [x8]
                   0xf100'0673U,    // subs x19, x19, #1
                   0x54ff'ffc1U,    // b.ne start
                   system_call},
      .add_offset = 4,
      .swap_mask = 0xffffU,
      .add_shift = 32,
      .add_mask = 0xffffU,
  });
}

} // namespace

int main() {
  require(uml::cpu::arm64::set_jit_cache_budget(
      256U * 1024U * 1024U, 10));
  native_single_block_case();
  native_linked_loop_case();
  for (unsigned size = 0; size < 4U; ++size) {
    for (bool acquire_release : {false, true}) {
      native_compare_retry_case(size, acquire_release, true);
      native_compare_retry_case(size, acquire_release, false);
      native_compare_retry_case(size, acquire_release, false, true);
    }
  }
  compare_lookahead_preserves_fetch_faults(false);
  compare_lookahead_preserves_fetch_faults(true);
  concurrent_compare_retry_case(false);
  concurrent_compare_retry_case(true);
  same_backing_alias_case(true);
  same_backing_alias_case(false);
  same_backing_alias_case(true, true);
  same_backing_alias_case(false, true);
  for (const auto store : {AliasStore::release, AliasStore::limited_release}) {
    same_backing_alias_case(true, false, store);
    same_backing_alias_case(false, false, store);
  }
  native_store_invalidates_semantic_reservation_case();
  slow_store_invalidates_semantic_reservation_case(false);
  slow_store_invalidates_semantic_reservation_case(true);
  concurrent_native_cold_paths_leave_publication_before_helper();
  helper_pair_compare_swap_keeps_native_updates();
  helper_misaligned_compare_swap_keeps_native_updates();
  return 0;
}
