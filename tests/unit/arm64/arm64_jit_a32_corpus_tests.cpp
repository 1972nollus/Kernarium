// SPDX-License-Identifier: GPL-2.0-or-later
/* The AArch32 instruction corpus on the JIT: every case of the pack that the
 * JIT's level implements runs through CpuEngine::run_aarch32 on the oracle's
 * memory map, and its final registers, APSR, TPIDRs, data bytes, event, fault
 * address and retired count are compared with what the oracle observed.
 * tests/unit/arm64/aarch32_corpus/README.md owns the pack and its format.
 *
 *   uml_arm64_jit_a32_corpus_tests PACK [--record NAME] [--case NAME]
 *                                       [--list-failures]
 */
#include "aarch32_corpus_pack.hpp"
#include "arm64_jit_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/jit/a32/a32_operations.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#ifndef DARWIN_UML_GUEST_PAGE_SIZE
#error "the guest page profile selects the corpus records"
#endif

namespace {

using namespace ::uml::emulator::tests;
using namespace ::uml::emulator::tests::a32;
namespace dec = ::uml::cpu::arm64::aarch32;
using ::uml::cpu::ExitReason;

using namespace ::uml::testing::aarch32_corpus;

/* The oracle's exit stubs load the address of one of its two epilogues into
 * the PC. Here each epilogue is an svc, which ends the run, and the epilogue
 * reached tells whether the program finished in A32 or in Thumb state (the
 * Thumb stub's target has bit 0 clear, so both epilogues run in A32 state). */

// --- The machine ----------------------------------------------------------------

class Machine final {
public:
  explicit Machine(unsigned long generation)
      : generation_{generation}, code_{kCodeSize / guest_page},
        data_{kDataSize / guest_page}, read_only_{kReadOnlySize / guest_page},
        engine_{::uml::cpu::make_cpu_engine(::uml::cpu::EngineKind::jit)} {
    require(darwin_uml_static_vm_activate(generation_) == 0);
    map(kCodeBase, code_, kCodeSize, PROT_READ | PROT_WRITE | PROT_EXEC);
    map(kDataBase, data_, kDataSize, PROT_READ | PROT_WRITE);
    map(kReadOnlyBase, read_only_, kReadOnlySize, PROT_READ);
    const auto fill = read_only_fill();
    read_only_.put(0, std::as_bytes(std::span{fill}));
    require(engine_ != nullptr);
  }
  ~Machine() { require(darwin_uml_static_vm_destroy(generation_) == 0); }
  Machine(const Machine &) = delete;
  Machine &operator=(const Machine &) = delete;

  /* Lays the case out; returns the address of the exit stub after the program
   * and, through `padded`, whether a nop precedes it. */
  void load(const Case &test, std::uint32_t &stub_address, bool &padded) {
    const bool thumb = test.flags & kFlagThumb;
    std::vector<std::byte> code(kCodeSize);
    const auto put16 = [&](std::uint32_t offset, std::uint16_t value) {
      std::memcpy(code.data() + offset, &value, 2);
    };
    const auto put32 = [&](std::uint32_t offset, std::uint32_t value) {
      std::memcpy(code.data() + offset, &value, 4);
    };
    for (std::size_t index = 0; index < test.halves.size(); ++index)
      put16(static_cast<std::uint32_t>(2 * index), test.halves[index]);
    std::uint32_t end = static_cast<std::uint32_t>(2 * test.halves.size());
    padded = false;
    const auto arm_stub = [&](std::uint32_t at) {
      put32(at, 0xe51ff004U); // ldr pc, [pc, #-4]
      put32(at + 4, kArmEpilogue);
    };
    const auto thumb_stub = [&](std::uint32_t at) {
      put16(at, 0xf8df); // ldr.w pc, [pc, #0]
      put16(at + 2, 0xf000);
      put32(at + 4, kThumbEpilogue);
    };
    if (thumb) {
      if (end % 4 != 0) {
        padded = true;
        put16(end, 0xbf00); // nop, so the stub is word aligned
        end += 2;
      }
      thumb_stub(end);
    } else {
      arm_stub(end);
    }
    arm_stub(kArmExit - kCodeBase);
    thumb_stub(kThumbExit - kCodeBase);
    put32(kArmEpilogue - kCodeBase, 0xef0000ffU);
    put32(kThumbEpilogue - kCodeBase, 0xef0000ffU);
    stub_address = kCodeBase + end;
    code_.put(0, code);
    /* The program replaces the last case's: retire the blocks translated
     * from it. */
    require(darwin_uml_static_vm_invalidate_code_generation(
                generation_, kCodeBase, kCodeSize) == 0);

    auto data = data_fill(test.fill);
    for (const auto &patch : test.data_patches)
      std::copy(patch.bytes.begin(), patch.bytes.end(),
                data.begin() + patch.offset);
    data_.put(0, std::as_bytes(std::span{data}));
    expected_data_.assign(reinterpret_cast<const std::byte *>(data.data()),
                          reinterpret_cast<const std::byte *>(data.data()) +
                              data.size());
  }

  [[nodiscard]] std::vector<std::byte> expected_memory(const Case &test) const {
    auto memory = expected_data_;
    for (const auto &patch : test.memory_patches)
      for (std::size_t index = 0; index < patch.bytes.size(); ++index)
        memory[patch.offset + index] = static_cast<std::byte>(patch.bytes[index]);
    return memory;
  }
  [[nodiscard]] std::vector<std::byte> read_data() const {
    std::vector<std::byte> bytes(kDataSize);
    data_.get(0, bytes);
    return bytes;
  }

  ::uml::cpu::ExecutionResult run(arm64::CpuState &cpu) {
    ::uml::memory::StaticGuestMemory memory;
    const auto sequences = darwin_uml_static_vm_sequences(generation_);
    memory.prepare(generation_, sequences.mapping_changes);
    ::uml::cpu::ExecutionRequest request{
        .cpu = cpu,
        .memory = memory,
        .translated_memory = &memory,
        .generation = generation_,
        .instruction_budget = 4096,
    };
    return engine_->run_aarch32(request);
  }

private:
  void map(std::uint32_t address, JitBacking &backing, std::size_t size, int prot) {
    require(darwin_uml_static_vm_map_generation(
                generation_, reinterpret_cast<void *>(std::uintptr_t{address}),
                backing.descriptor(), 0, size, prot, 1, 1) == 0);
  }

  unsigned long generation_;
  JitBacking code_;
  JitBacking data_;
  JitBacking read_only_;
  std::unique_ptr<::uml::cpu::CpuEngine> engine_;
  std::vector<std::byte> expected_data_;
};

// --- Checking -------------------------------------------------------------------

arm64::CpuState cpu_from(const State &state, bool thumb) {
  arm64::CpuState cpu{};
  cpu.a32 = 1;
  cpu.pc = thumb ? key::t32_key(kCodeBase) : key::a32_key(kCodeBase);
  for (unsigned index = 0; index < kRegisters; ++index)
    cpu.registers[index] = state.r[index];
  cpu.nzcv = state.apsr & 0xf000'0000U;
  cpu.a32_q = static_cast<std::uint8_t>(state.apsr >> 27 & 1U);
  for (unsigned lane = 0; lane < 4; ++lane)
    if (state.apsr >> (16 + lane) & 1U)
      cpu.a32_ge_mask |= 0xffU << (8 * lane);
  cpu.a32_cp15ben = 1;
  cpu.a32_tpidruro = state.tpidruro;
  cpu.tls = state.tpidrurw;
  return cpu;
}

std::uint32_t apsr_of(const arm64::CpuState &cpu) {
  std::uint32_t apsr = static_cast<std::uint32_t>(cpu.nzcv) & 0xf000'0000U;
  if (cpu.a32_q != 0U)
    apsr |= 1U << 27;
  for (unsigned lane = 0; lane < 4; ++lane)
    if ((cpu.a32_ge_mask >> (8 * lane) & 0xffU) != 0U)
      apsr |= 1U << (16 + lane);
  return apsr;
}

struct Totals final {
  std::size_t run = 0, skipped = 0, failed = 0, retried = 0;
  std::map<std::string, std::size_t> failed_forms;
};

/* Returns the reason the JIT disagrees with the oracle, or nothing. */
std::string check(Machine &machine, const Case &test) {
  const bool thumb = test.flags & kFlagThumb;
  std::uint32_t stub = 0;
  bool padded = false;
  machine.load(test, stub, padded);
  auto cpu = cpu_from(test.initial, thumb);
  const auto result = machine.run(cpu);

  char text[512];
  const auto fail = [&](const char *what) {
    std::snprintf(text, sizeof text, "%s (reason %d, retired %zu, pc %016llx)",
                  what, static_cast<int>(result.reason), result.retired_instructions,
                  static_cast<unsigned long long>(cpu.pc));
    return std::string{text};
  };

  const std::uint32_t pc = key::key_pc(cpu.pc);
  const bool at_thumb = key::key_is_thumb(cpu.pc);
  unsigned event = 0;
  std::size_t retired = result.retired_instructions;
  switch (result.reason) {
  case ExitReason::system_call: {
    // pc is past the svc: a stub's svc is a finished program.
    const std::uint32_t svc = pc - (at_thumb ? 2U : 4U);
    if (!at_thumb && (svc == kArmEpilogue || svc == kThumbEpilogue)) {
      event = 0;
      if (retired < 2)
        return fail("the stub exit retired too little");
      retired -= 2; // the stub's load of the PC and its svc
      if (padded && (test.flags & kFlagStraight) != 0)
        --retired; // the nop that aligns a Thumb stub the program fell into
      if (((test.flags & kFlagExitThumb) != 0) != (svc == kThumbEpilogue))
        return fail("finished in the wrong instruction set");
    } else {
      event = kEventSvc;
      --retired; // the program's own svc retires but the corpus does not count it
    }
    break;
  }
  case ExitReason::illegal_instruction: {
    event = kSigIll;
    /* The three undefined encodings Linux turns into SIGTRAP (arm64's compat
     * layer, as arm's): A32 0xe7f001f0, T16 0xde01 and T32 0xf7f0a000. */
    std::uint32_t word = 0;
    const std::uint32_t at = pc;
    const auto read16 = [&](std::uint32_t address) {
      const std::uint32_t index = address - kCodeBase;
      return static_cast<std::uint32_t>(test.halves[index / 2]);
    };
    if (at >= kCodeBase && at - kCodeBase < 2 * test.halves.size()) {
      if (at_thumb) {
        word = read16(at);
        if (word == 0xde01U)
          event = kSigTrap;
        else if (word == 0xf7f0U && at + 2 - kCodeBase < 2 * test.halves.size() &&
                 read16(at + 2) == 0xa000U)
          event = kSigTrap;
      } else if (at + 4 - kCodeBase <= 2 * test.halves.size()) {
        word = read16(at) | read16(at + 2) << 16;
        if (word == 0xe7f001f0U)
          event = kSigTrap;
      }
    }
    break;
  }
  case ExitReason::breakpoint:
    event = kSigTrap;
    --retired; // a breakpoint retires in the engines' accounting, not in the corpus
    break;
  case ExitReason::memory_fault:
    event = result.fault.alignment ? kSigBus : kSigSegv;
    break;
  case ExitReason::pc_alignment: event = kSigBus; break;
  default:
    return fail("unexpected exit reason");
  }
  if (event != test.event) {
    std::snprintf(text, sizeof text, "event %u, expected %u (reason %d, retired %zu)",
                  event, test.event, static_cast<int>(result.reason),
                  result.retired_instructions);
    return text;
  }
  if (retired != test.retired) {
    std::snprintf(text, sizeof text, "retired %zu (engine %zu), expected %u", retired,
                  result.retired_instructions, test.retired);
    return text;
  }
  if (event == kSigIll || event == kSigTrap) {
    if (pc != test.fault_address) {
      std::snprintf(text, sizeof text, "fault pc %08x, expected %08x", pc,
                    test.fault_address);
      return text;
    }
  } else if (event == kSigSegv || event == kSigBus) {
    if (result.fault.address != test.fault_address) {
      std::snprintf(text, sizeof text, "fault address %08llx, expected %08x",
                    static_cast<unsigned long long>(result.fault.address),
                    test.fault_address);
      return text;
    }
  }
  for (unsigned index = 0; index < kRegisters; ++index) {
    if (static_cast<std::uint32_t>(cpu.registers[index]) != test.final.r[index] ||
        (cpu.registers[index] >> 32) != 0) {
      std::snprintf(text, sizeof text, "r%u = %016llx, expected %08x", index,
                    static_cast<unsigned long long>(cpu.registers[index]),
                    test.final.r[index]);
      return text;
    }
  }
  if (apsr_of(cpu) != test.final.apsr) {
    std::snprintf(text, sizeof text, "apsr %08x, expected %08x", apsr_of(cpu),
                  test.final.apsr);
    return text;
  }
  if (cpu.a32_tpidruro != test.final.tpidruro || cpu.tls != test.final.tpidrurw)
    return fail("a thread register changed");
  const auto expected = machine.expected_memory(test);
  const auto actual = machine.read_data();
  if (std::memcmp(expected.data(), actual.data(), kDataSize) != 0) {
    std::size_t at = 0;
    while (expected[at] == actual[at])
      ++at;
    std::snprintf(text, sizeof text, "data differs at +%zx: %02x, expected %02x", at,
                  static_cast<unsigned>(actual[at]),
                  static_cast<unsigned>(expected[at]));
    return text;
  }
  return {};
}

/* Whether the JIT's level implements every instruction of the program. */
bool within_level(const Case &test, const dec::Profile &profile) {
  const bool thumb = test.flags & kFlagThumb;
  dec::ItState it;
  for (unsigned half = 0; half < test.halves.size();) {
    dec::Instruction instruction;
    if (!thumb) {
      if (half + 1 >= test.halves.size())
        break;
      instruction = dec::decode_a32(
          std::uint32_t{test.halves[half + 1]} << 16 | test.halves[half], profile);
      half += 2;
    } else {
      const std::uint16_t first = test.halves[half];
      const unsigned length = dec::thumb_length(first);
      if (half + length / 2 > test.halves.size())
        break;
      const std::uint32_t word = length == 4
                                     ? std::uint32_t{first} << 16 | test.halves[half + 1]
                                     : std::uint32_t{first} << 16;
      instruction = dec::decode_thumb(word, it, profile);
      it = instruction.next_it;
      half += length / 2;
    }
    if (instruction.legal() &&
        static_cast<int>(instruction.info().level) >
            static_cast<int>(::uml::cpu::arm64::jit::a32::a32_level()))
      return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  const char *path = nullptr;
  std::string only_record;
  std::string only_case;
  bool list_failures = false;
  bool trace = false;
  bool all_failures = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--record" && index + 1 < argc)
      only_record = argv[++index];
    else if (argument == "--case" && index + 1 < argc)
      only_case = argv[++index];
    else if (argument == "--list-failures")
      list_failures = true;
    else if (argument == "--trace")
      trace = true;
    else if (argument == "--all-failures")
      all_failures = true;
    else
      path = argv[index];
  }
  require(path != nullptr);
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  const auto pack = read_pack(read_file(path));
  const auto profile = dec::Profile::at(dec::Level::complete);

  Machine machine{0x1a32'0300};
  Totals totals;
  for (const auto &record : pack.records) {
    if ((record.profiles & kProfileBit) == 0 ||
        (!only_record.empty() && record.name != only_record))
      continue;
    for (const auto &test : record.cases) {
      if (!only_case.empty() && test.name != only_case)
        continue;
      if (!within_level(test, profile)) {
        ++totals.skipped;
        continue;
      }
      ++totals.run;
      if (trace) {
        std::fprintf(stderr, "case %s:", test.name.c_str());
        for (const auto half : test.halves)
          std::fprintf(stderr, " %04x", half);
        std::fprintf(stderr, "\n");
      }
      auto problem = check(machine, test);
      /* A store-exclusive may fail without cause (the architecture allows
       * it, and the host's does on the first fused pair a process runs), and
       * the oracle's never does. A guest retries; a lone corpus case cannot,
       * so a failing exclusive case runs once more and must then agree
       * completely. */
      if (!problem.empty() && record.name.starts_with("exclusive")) {
        problem = check(machine, test);
        if (problem.empty()) {
          ++totals.retried;
          continue;
        }
      }
      if (problem.empty())
        continue;
      ++totals.failed;
      ++totals.failed_forms[test.form];
      if (totals.failed_forms[test.form] == 1 || all_failures || !only_case.empty()) {
        std::fprintf(stderr, "FAIL %s: %s: %s\n  program:", record.name.c_str(),
                     test.name.c_str(), problem.c_str());
        for (const auto half : test.halves)
          std::fprintf(stderr, " %04x", half);
        std::fprintf(stderr, "%s\n", test.flags & kFlagThumb ? " (thumb)" : " (arm)");
      }
    }
  }
  std::printf("AArch32 corpus on the JIT: %zu cases run, %zu skipped below the level, "
              "%zu failed (%zu exclusive retried)\n",
              totals.run, totals.skipped, totals.failed,
              totals.retried);
  if (list_failures || totals.failed != 0)
    for (const auto &[form, count] : totals.failed_forms)
      std::printf("  %s: %zu\n", form.c_str(), count);
  return totals.failed == 0 ? 0 : 1;
}
