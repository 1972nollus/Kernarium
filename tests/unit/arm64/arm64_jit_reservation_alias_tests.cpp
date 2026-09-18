// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_semantic_memory.hpp"

#include <array>
#include <atomic>
#include <barrier>
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
      cpu.mte_enabled = mixed_semantic && index == 0;
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
  first_cpu.mte_enabled = true;
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
  semantic_cpu.mte_enabled = true;
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
  concurrent_native_cold_paths_leave_publication_before_helper();
  return 0;
}
