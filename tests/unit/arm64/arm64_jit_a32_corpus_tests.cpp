// SPDX-License-Identifier: GPL-2.0-or-later
/* The AArch32 instruction corpus on the JIT: every case of the pack that the
 * JIT's level implements runs through CpuEngine::run_aarch32 on the oracle's
 * memory map, and its final registers, APSR, TPIDRs, data bytes, event, fault
 * address and retired count are compared with what the oracle observed.
 * tests/unit/arm64/aarch32_corpus/README.md owns the pack and its format.
 *
 *   uml_arm64_jit_a32_corpus_tests PACK [--record NAME] [--case NAME]
 *                                       [--form PREFIX] [--list-failures]
 */
#include "aarch32_corpus_pack.hpp"
#include "arm64_jit_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/jit/a32/a32_compiler.hpp"
#include "cpu/arm64/jit/a32/a32_helpers.hpp"
#include "cpu/arm64/jit/a32/a32_operations.hpp"
#include "cpu/arm64/jit/a32/a32_vector_operations.hpp"

#include <algorithm>
#include <array>
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

/* FPSCR's control bits (AHP, DN, FZ, RMode, FZ16) and its status bits (QC and
 * the cumulative flags); N, Z, C and V are a32_fpscr_nzcv. */
constexpr std::uint32_t fpscr_control_mask = 0x07c8'0000U;
constexpr std::uint32_t fpscr_status_mask = 0x0800'009fU;

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
      put16(test.origin + static_cast<std::uint32_t>(2 * index),
            test.halves[index]);
    std::uint32_t end =
        test.origin + static_cast<std::uint32_t>(2 * test.halves.size());
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
    /* A program at the end of the region has no room for a stub and falls
     * off it. */
    if (thumb) {
      if (end % 4 != 0 && end + 2 + 8 <= kCodeSize) {
        padded = true;
        put16(end, 0xbf00); // nop, so the stub is word aligned
        end += 2;
      }
      if (end + 8 <= kCodeSize)
        thumb_stub(end);
    } else if (end + 8 <= kCodeSize) {
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

arm64::CpuState cpu_from(const State &state, bool thumb,
                         std::uint32_t entry = kCodeBase) {
  arm64::CpuState cpu{};
  cpu.a32 = 1;
  cpu.pc = thumb ? key::t32_key(entry) : key::a32_key(entry);
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
  for (unsigned d = 0; d < kVectors; ++d)
    std::memcpy(cpu.vectors[d / 2].bytes.data() + 8U * (d & 1U), &state.d[d], 8);
  /* FPSCR is split as the CpuState holds it. */
  cpu.fpcr = state.fpscr & fpscr_control_mask;
  cpu.fpsr = state.fpscr & fpscr_status_mask;
  cpu.a32_fpscr_nzcv = state.fpscr & 0xf000'0000U;
  return cpu;
}

std::uint32_t fpscr_of(const arm64::CpuState &cpu) {
  return (cpu.fpcr & fpscr_control_mask) | (cpu.fpsr & fpscr_status_mask) |
         (cpu.a32_fpscr_nzcv & 0xf000'0000U);
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
  std::size_t run = 0, skipped = 0, pending = 0, failed = 0, retried = 0;
  std::map<std::string, std::size_t> failed_forms;
};

/* Returns the reason the JIT disagrees with the oracle, or nothing. */
std::string check(Machine &machine, const Case &test) {
  const bool thumb = test.flags & kFlagThumb;
  std::uint32_t stub = 0;
  bool padded = false;
  machine.load(test, stub, padded);
  auto cpu = cpu_from(test.initial, thumb, kCodeBase + test.origin);
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
    const std::uint32_t program = kCodeBase + test.origin;
    const auto read16 = [&](std::uint32_t address) {
      const std::uint32_t index = address - program;
      return static_cast<std::uint32_t>(test.halves[index / 2]);
    };
    if (at >= program && at - program < 2 * test.halves.size()) {
      if (at_thumb) {
        word = read16(at);
        if (word == 0xde01U)
          event = kSigTrap;
        else if (word == 0xf7f0U && at + 2 - program < 2 * test.halves.size() &&
                 read16(at + 2) == 0xa000U)
          event = kSigTrap;
      } else if (at + 4 - program <= 2 * test.halves.size()) {
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
  for (unsigned d = 0; d < kVectors; ++d) {
    std::uint64_t value;
    std::memcpy(&value, cpu.vectors[d / 2].bytes.data() + 8U * (d & 1U), 8);
    if (value != test.final.d[d]) {
      std::snprintf(text, sizeof text, "d%u = %016llx, expected %016llx", d,
                    static_cast<unsigned long long>(value),
                    static_cast<unsigned long long>(test.final.d[d]));
      return text;
    }
  }
  if (fpscr_of(cpu) != test.final.fpscr) {
    std::snprintf(text, sizeof text, "fpscr %08x, expected %08x", fpscr_of(cpu),
                  test.final.fpscr);
    return text;
  }
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
    const unsigned first_half = half;
    const dec::ItState before_it = it;
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
    /* An encoding the oracle executes but the JIT's own profile leaves out
     * (the half-precision variants of the simd-level forms need FP16). */
    if (instruction.legal()) {
      const auto own = dec::Profile::at(::uml::cpu::arm64::jit::a32::a32_level());
      const auto word_at = [&](unsigned at) {
        return thumb ? (dec::thumb_length(test.halves[at]) == 4
                            ? std::uint32_t{test.halves[at]} << 16 | test.halves[at + 1]
                            : std::uint32_t{test.halves[at]} << 16)
                     : std::uint32_t{test.halves[at + 1]} << 16 | test.halves[at];
      };
      const auto again = thumb ? dec::decode_thumb(word_at(first_half), before_it, own)
                               : dec::decode_a32(word_at(first_half), own);
      if (!again.legal())
        return false;
    }
  }
  return true;
}

/* Whether the compiler lowers every simd-level instruction of the program. */
bool lowered(const Case &test, const dec::Profile &profile) {
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
    if (instruction.legal() && instruction.info().level == dec::Level::simd &&
        !::uml::cpu::arm64::jit::a32::vector_lowered(instruction.form))
      return false;
  }
  return true;
}

/* Whether the instruction under test always executes in the case: a
 * straight-line program and an unconditional instruction. */
bool target_always_executes(const Case &test, const dec::Profile &profile) {
  if ((test.flags & kFlagStraight) == 0)
    return false;
  const bool thumb = test.flags & kFlagThumb;
  dec::ItState it;
  unsigned ordinal = 0;
  for (unsigned half = 0; half < test.halves.size(); ++ordinal) {
    dec::Instruction instruction;
    unsigned length = 4;
    if (!thumb) {
      if (half + 1 >= test.halves.size())
        return false;
      instruction = dec::decode_a32(
          std::uint32_t{test.halves[half + 1]} << 16 | test.halves[half], profile);
    } else {
      const std::uint16_t first = test.halves[half];
      length = dec::thumb_length(first);
      if (half + length / 2 > test.halves.size())
        return false;
      const std::uint32_t word = length == 4
                                     ? std::uint32_t{first} << 16 | test.halves[half + 1]
                                     : std::uint32_t{first} << 16;
      instruction = dec::decode_thumb(word, it, profile);
      it = instruction.next_it;
    }
    if (ordinal == test.target)
      return instruction.cond >= 14U;
    half += length / 2;
  }
  return false;
}

/* The library-exit gate: a form the compiler lowers stays out of the slow
 * helper from a clean state. Every event-free case of a form runs twice (the
 * first run warms the translation and the TLB) and the second run's helper
 * exits for the form under test are counted. */
struct ExitTally final {
  /* Every event-free case, and those that took no helper exit. */
  std::size_t cases = 0, clean = 0;
  /* The same over the cases whose instruction under test always executes. */
  std::size_t executed = 0, executed_clean = 0;
};

std::map<std::string, ExitTally> measure_helper_exits(
    Machine &machine, const Pack &pack, const dec::Profile &profile,
    bool report) {
  std::map<std::string, dec::Form> forms;
  for (std::size_t index = 0; index < dec::kFormCount; ++index)
    forms.emplace(dec::kForms[index].name, static_cast<dec::Form>(index));
  std::map<std::string, ExitTally> tallies;
  ::uml::cpu::arm64::jit::a32::count_helper_exits(true);
  for (const auto &record : pack.records) {
    if ((record.profiles & kProfileBit) == 0)
      continue;
    for (const auto &test : record.cases) {
      if (test.event != 0 || !within_level(test, profile))
        continue;
      const auto form = forms.find(test.form);
      require(form != forms.end());
      const bool thumb = test.flags & kFlagThumb;
      std::uint32_t stub = 0;
      bool padded = false;
      std::size_t before = 0;
      for (unsigned pass = 0; pass < 2; ++pass) {
        machine.load(test, stub, padded);
        auto cpu = cpu_from(test.initial, thumb, kCodeBase + test.origin);
        before = ::uml::cpu::arm64::jit::a32::helper_exits(form->second);
        (void)machine.run(cpu);
      }
      const auto taken =
          ::uml::cpu::arm64::jit::a32::helper_exits(form->second) - before;
      auto &tally = tallies[test.form];
      ++tally.cases;
      tally.clean += taken == 0 ? 1U : 0U;
      if (target_always_executes(test, profile)) {
        ++tally.executed;
        tally.executed_clean += taken == 0 ? 1U : 0U;
      }
      if (report && taken != 0)
        std::fprintf(stderr, "exit %s %s/%s: %zu\n", test.form.c_str(),
                     record.name.c_str(), test.name.c_str(),
                     static_cast<std::size_t>(taken));
    }
  }
  ::uml::cpu::arm64::jit::a32::count_helper_exits(false);
  return tallies;
}

/* Requires every form the compiler lowers to stay out of the helper from a
 * clean state: some case in which it always executes takes no helper exit
 * (the others cross a page, write back over the loaded register and so on).
 * The forms the library always runs take it every time, which keeps the
 * predicate that names it honest, and the exclusives are measured by their
 * own test. */
void require_lowered_forms_stay_inline(Machine &machine, const Pack &pack,
                                       const dec::Profile &profile) {
  const auto tallies = measure_helper_exits(machine, pack, profile, false);
  std::size_t inline_forms = 0, helper_forms = 0;
  for (const auto &[name, tally] : tallies) {
    dec::Form form{};
    for (std::size_t index = 0; index < dec::kFormCount; ++index)
      if (name == dec::kForms[index].name)
        form = static_cast<dec::Form>(index);
    const auto op = ::uml::cpu::arm64::jit::a32::op_info(form).op;
    using ::uml::cpu::arm64::jit::a32::Op;
    if (::uml::cpu::arm64::jit::a32::is_exclusive_op(op))
      continue;
    /* The cases that decide: those in which the instruction executes, or all
     * of them for a form whose cases are all conditional. */
    const auto decisive = tally.executed != 0 ? tally.executed : tally.cases;
    const auto decisive_clean =
        tally.executed != 0 ? tally.executed_clean : tally.clean;
    if (::uml::cpu::arm64::jit::a32::is_library_form(form)) {
      if (decisive_clean != 0) {
        std::fprintf(stderr, "%s runs inline but is named a library form\n",
                     name.c_str());
        require(false);
      }
      ++helper_forms;
      continue;
    }
    if (decisive_clean == 0) {
      std::fprintf(stderr,
                   "%s is lowered but took the helper from a clean state in "
                   "all %zu cases\n",
                   name.c_str(), decisive);
      require(false);
    }
    ++inline_forms;
  }
  std::printf("%zu lowered forms stay inline from a clean state; %zu run in "
              "the semantic library\n",
              inline_forms, helper_forms);
}

} // namespace

/* --dump WORD [HALFWORD2]: translates one A32 word (or T32 pair) and prints
 * the generated instructions as assembler directives. */
void dump_translation(std::uint32_t word, bool thumb) {
  using namespace ::uml::cpu::arm64::jit::a32;
  A32Compiler compiler;
  std::array<std::byte, 16> bytes{};
  const std::uint32_t nop = 0xe320f000U;
  if (thumb) {
    const std::uint16_t first = static_cast<std::uint16_t>(word >> 16);
    const std::uint16_t second = static_cast<std::uint16_t>(word);
    std::memcpy(bytes.data(), &first, 2);
    std::memcpy(bytes.data() + 2, &second, 2);
  } else {
    std::memcpy(bytes.data(), &word, 4);
    std::memcpy(bytes.data() + 4, &nop, 4);
  }
  Request request;
  request.key = thumb ? ::uml::cpu::arm64::aarch32::t32_key(0x1000)
                      : ::uml::cpu::arm64::aarch32::a32_key(0x1000);
  request.bytes = bytes;
  request.single = true;
  const auto result = compiler.compile(request);
  for (const auto instruction : result.translation.words)
    std::printf(".inst 0x%08x\n", instruction);
}

int main(int argc, char **argv) {
  const char *path = nullptr;
  std::string only_record;
  std::string only_case;
  bool list_failures = false;
  bool trace = false;
  bool all_failures = false;
  bool exit_report = false;
  std::string only_form;
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
    else if (argument == "--exit-report")
      exit_report = true;
    else if (argument == "--form" && index + 1 < argc)
      only_form = argv[++index];
    else if (argument == "--dump" && index + 1 < argc) {
      const std::uint32_t word =
          static_cast<std::uint32_t>(std::strtoul(argv[++index], nullptr, 16));
      const bool thumb = index + 1 < argc && std::string_view{argv[index + 1]} == "thumb";
      dump_translation(word, thumb);
      return 0;
    }
    else
      path = argv[index];
  }
  require(path != nullptr);
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  const auto pack = read_pack(read_file(path));
  const auto profile = dec::Profile::at(dec::Level::complete);

  Machine machine{0x1a32'0300};
  if (exit_report) {
    const auto tallies = measure_helper_exits(machine, pack, profile, true);
    for (const auto &[form, tally] : tallies)
      if (tally.clean != tally.cases)
        std::printf("%s: %zu of %zu cases clean\n", form.c_str(), tally.clean,
                    tally.cases);
    return 0;
  }
  Totals totals;
  for (const auto &record : pack.records) {
    if ((record.profiles & kProfileBit) == 0 ||
        (!only_record.empty() && record.name != only_record))
      continue;
    for (const auto &test : record.cases) {
      if (!only_case.empty() && test.name != only_case)
        continue;
      if (!only_form.empty() && !test.form.starts_with(only_form))
        continue;
      if (!within_level(test, profile)) {
        ++totals.skipped;
        continue;
      }
      if (!lowered(test, profile)) {
        ++totals.pending;
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
      if (!problem.empty() && record.name.find("exclusive") != std::string::npos) {
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
              "%zu pending, %zu failed (%zu exclusive retried)\n",
              totals.run, totals.skipped, totals.pending, totals.failed,
              totals.retried);
  if (list_failures || totals.failed != 0)
    for (const auto &[form, count] : totals.failed_forms)
      std::printf("  %s: %zu\n", form.c_str(), count);
  if (totals.failed == 0 && only_record.empty() && only_case.empty() &&
      only_form.empty())
    require_lowered_forms_stay_inline(machine, pack, profile);
  return totals.failed == 0 ? 0 : 1;
}
