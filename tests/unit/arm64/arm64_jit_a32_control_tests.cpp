// SPDX-License-Identifier: GPL-2.0-or-later
/* The JIT's AArch32 control behaviour: IT blocks entered from the start, one
 * instruction at a time and from the middle; blocks longer than the compiler
 * keeps in one translation and the instruction budget across them; rewritten
 * code (A32 words, T32 halfwords of a 32-bit instruction, one byte) seen by
 * the translations that held the old code; and the complete level's optional
 * extensions, which decode only where the host has them. */
#include "arm64_jit_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/host_capabilities.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <span>
#include <vector>

using namespace uml::emulator::tests;
using namespace uml::emulator::tests::a32;
using ::uml::cpu::ExitReason;
using ::uml::platform::ArmFeature;
using ::uml::platform::HostCapabilities;

namespace {

/* cmp r4, #0; ittte eq; moveq r0,#1; moveq r1,#2; moveq r2,#3; movne r3,#4;
 * ite ne; addne r5,#1; addeq r5,#2; svc. */
constexpr std::array<std::uint16_t, 10> it_program{
    0x2c00, 0xbf03, 0x2001, 0x2102, 0x2203, 0x2304, 0xbf14, 0x1c6d, 0x1cad, 0xdf00};
/* The second conditional instruction's offset, in bytes. */
constexpr unsigned second_move = 6;

arm64::CpuState it_task(std::uint32_t r4) {
  auto cpu = Fixture::task(true);
  cpu.registers[4] = r4;
  cpu.registers[5] = 100;
  cpu.registers[3] = 0x33;
  return cpu;
}

void check_final(const arm64::CpuState &cpu, bool equal) {
  require(cpu.registers[0] == (equal ? 1U : 0U));
  require(cpu.registers[1] == (equal ? 2U : 0U));
  require(cpu.registers[2] == (equal ? 3U : 0U));
  require(cpu.registers[3] == (equal ? 0x33U : 4U));
  require(cpu.registers[5] == (equal ? 102U : 101U));
  require(cpu.a32_itstate == 0);
  require(cpu.pc == key::t32_key(code_base + 2 * it_program.size()));
}

void it_blocks_whole_stepped_and_entered_mid_block() {
  Fixture fixture{0x1a32'0c01};
  for (std::size_t index = 0; index < it_program.size(); ++index)
    fixture.code16(2 * index, {it_program[index]});

  for (const bool equal : {true, false}) {
    // The whole block.
    auto cpu = it_task(equal ? 0 : 1);
    auto result = fixture.run(cpu);
    require(result.reason == ExitReason::system_call);
    require(result.retired_instructions == it_program.size());
    check_final(cpu, equal);

    // One instruction at a time: ITSTATE is live exactly inside the blocks.
    cpu = it_task(equal ? 0 : 1);
    std::vector<std::uint8_t> itstates;
    std::size_t retired = 0;
    for (;;) {
      result = fixture.run(cpu, 1, true);
      retired += result.retired_instructions;
      if (result.reason == ExitReason::system_call)
        break;
      require(result.reason == ExitReason::single_step);
      require(result.retired_instructions == 1);
      itstates.push_back(cpu.a32_itstate);
      require(itstates.size() < 20);
    }
    require(retired == it_program.size());
    check_final(cpu, equal);
    require(itstates.size() == it_program.size() - 1);
    require(itstates[0] == 0);   // after the cmp
    require(itstates[1] != 0);   // after ittte
    require(itstates[2] != 0);   // after the first move
    require(itstates[3] != 0);   // after the second
    require(itstates[4] != 0);   // after the third
    require(itstates[5] == 0);   // after the last of the block
    require(itstates[6] != 0);   // after ite
    require(itstates[7] != 0);
    require(itstates[8] == 0);

    // Entered in the middle of the first block: the state a step observed.
    cpu = it_task(equal ? 0 : 1);
    cpu.nzcv = equal ? flag_z : 0;
    cpu.pc = key::t32_key(code_base + second_move);
    cpu.a32_itstate = itstates[2];
    result = fixture.run(cpu);
    require(result.reason == ExitReason::system_call);
    require(result.retired_instructions == it_program.size() - 3);
    require(cpu.registers[0] == 0); // the first move was not executed
    require(cpu.registers[1] == (equal ? 2U : 0U));
    require(cpu.registers[2] == (equal ? 3U : 0U));
    require(cpu.registers[3] == (equal ? 0x33U : 4U));
    require(cpu.registers[5] == (equal ? 102U : 101U));
    require(cpu.a32_itstate == 0);
  }
}

void long_blocks_and_the_budget() {
  // Long enough that a chain (which carries on across the blocks of a straight
  // run, their exits being linked, until its allowance of 4096 instructions is
  // spent) ends before the program does. The code spans 12 pages in either
  // profile.
  Fixture fixture{0x1a32'0c02, 12};
  constexpr std::size_t adds = 10000;
  {
    std::vector<std::uint32_t> words(adds, 0xe280'0001U); // add r0, r0, #1
    words.push_back(0xef00'0000U);
    for (std::size_t index = 0; index < words.size(); ++index)
      fixture.code32(4 * index, {words[index]});
  }
  auto cpu = Fixture::task(false);
  auto result = fixture.run(cpu, 1u << 20);
  require(result.reason == ExitReason::system_call);
  require(result.retired_instructions == adds + 1);
  require(cpu.registers[0] == adds);

  // A small budget stops at a block boundary, past the budget, with exactly
  // the retired instructions' effects, and the next call resumes there.
  cpu = Fixture::task(false);
  std::size_t retired = 0;
  unsigned exits = 0;
  for (;;) {
    result = fixture.run(cpu, 50);
    retired += result.retired_instructions;
    if (result.reason == ExitReason::system_call)
      break;
    require(result.reason == ExitReason::instruction_budget);
    require(result.retired_instructions >= 50);
    require(cpu.registers[0] == retired);
    require(cpu.pc == key::a32_key(code_base + 4 * static_cast<std::uint32_t>(retired)));
    require(++exits < 1000);
  }
  require(exits >= 2);
  require(retired == adds + 1);
  require(cpu.registers[0] == adds);
}

/* Whether a program's blocks are one chain once they are all translated: the
 * blocks are run last first, so each one's exit finds a translated successor,
 * and the whole program then runs from its start as a single block entry. The
 * exits that join them are static: the fetch window's cut, the arm of a
 * conditional branch or CBZ that is not taken. */
struct ChainProgram final {
  std::vector<std::uint8_t> bytes;
  std::vector<std::size_t> starts; // offsets of the blocks after the first
  bool thumb;
  std::size_t retired;
  std::uint32_t r0;
};

void run_as_one_chain(unsigned long generation, const ChainProgram &program) {
  constexpr std::uint32_t base = 0x00f0'0000U;
  JitBacking backing{1};
  backing.put(0, std::as_bytes(std::span{program.bytes}));
  JitMapping mapping{generation, base, backing, 1};
  ::uml::cpu::arm64::JitExecutor executor;
  ::uml::memory::StaticGuestMemory memory;
  const auto entry = [&](std::size_t offset) {
    arm64::CpuState cpu{};
    cpu.a32 = 1;
    cpu.pc = program.thumb ? key::t32_key(base + static_cast<std::uint32_t>(offset))
                           : key::a32_key(base + static_cast<std::uint32_t>(offset));
    return cpu;
  };
  require(executor.begin_execution(generation, base));
  std::size_t executed = 0;
  for (auto start = program.starts.rbegin(); start != program.starts.rend();
       ++start) {
    auto cpu = entry(*start);
    (void)executor.run_a32_block(cpu, memory, generation, executed);
  }
  auto cpu = entry(0);
  const auto event = executor.run_a32_block(cpu, memory, generation, executed);
  require(event == ::uml::cpu::EngineEvent::system_call);
  require(executed == program.retired);
  require(cpu.registers[0] == program.r0);
  executor.end_execution();
}

void static_exits_chain() {
  constexpr std::uint32_t add_r0 = 0xe280'0001U; // add r0, r0, #1
  const auto a32_bytes = [](const std::vector<std::uint32_t> &words) {
    std::vector<std::uint8_t> bytes;
    for (const auto word : words)
      for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(word >> shift));
    return bytes;
  };

  // A straight run of more words than a block holds: the fetch window cuts it.
  {
    constexpr std::size_t window = 60; // the compiler's block length (a32_compiler.cpp)
    constexpr std::size_t words = 2 * window + 22;
    std::vector<std::uint32_t> program(words, add_r0);
    program.push_back(0xef00'0000U); // svc
    run_as_one_chain(0x1a32'0c10, {a32_bytes(program), {window * 4, 2 * window * 4},
                                   false, words + 1, static_cast<std::uint32_t>(words)});
  }
  // A conditional branch that is not taken: cmp r1, r1 sets Z, so bne falls
  // through to the next block; the skipped arm is a static successor.
  {
    const std::uint32_t cmp = 0xe151'0001U;  // cmp r1, r1
    const std::uint32_t bne = 0x1a00'0000U;  // bne .+8
    run_as_one_chain(
        0x1a32'0c11,
        {a32_bytes({cmp, bne, add_r0, cmp, bne, add_r0, cmp, bne, add_r0, 0xef00'0000U}),
         {8, 20, 32},
         false, 10, 3});
  }
  // CBZ that is not taken (T32): the fall-through is a static successor.
  {
    const std::vector<std::uint16_t> halves{
        0x2101,  // movs r1, #1
        0xb101,  // cbz r1, .+4 (not taken)
        0x3001,  // adds r0, #1
        0xb101,  // cbz r1, .+4
        0x3001,  // adds r0, #1
        0xdf00,  // svc
    };
    std::vector<std::uint8_t> bytes;
    for (const auto half : halves) {
      bytes.push_back(static_cast<std::uint8_t>(half));
      bytes.push_back(static_cast<std::uint8_t>(half >> 8));
    }
    run_as_one_chain(0x1a32'0c12, {bytes, {4, 8}, true, 6, 2});
  }
}

/* mov r0, #1 (T32 mov.w), nop, movw r8, #0x1234, svc. */
void rewritten_t32_halfwords() {
  Fixture fixture{0x1a32'0c03};
  fixture.code16(0, {0x2001, 0xbf00, 0xf241, 0x2834, 0xdf00});
  auto cpu = Fixture::task(true);
  auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[8] == 0x1234);

  // The second halfword of a 32-bit instruction alone.
  fixture.code16(6, {0x2835});
  cpu = Fixture::task(true);
  result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[8] == 0x1235);

  // The first halfword alone: movw r8 becomes movt r8 (0xf2c1).
  fixture.code16(4, {0xf2c1});
  cpu = Fixture::task(true);
  cpu.registers[8] = 0x0000'00aaU;
  result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[8] == 0x1235'00aaU);

  // A nop rewritten into the first half of a 32-bit instruction changes the
  // length of what follows it: movw r8, #0x1236, then cmp r0, #0x35.
  fixture.code16(2, {0xf241, 0x2836});
  cpu = Fixture::task(true);
  result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(result.retired_instructions == 4);
  require(cpu.registers[8] == 0x1236);
  require((cpu.nzcv & flag_n) != 0); // the old second half is now cmp r0, #0x35
}

void rewritten_a32_words() {
  Fixture fixture{0x1a32'0c04};
  fixture.code32(0, {0xe3a0'0001U, 0xe280'1002U, 0xe281'2003U, 0xef00'0000U});
  auto cpu = Fixture::task(false);
  auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[2] == 6);

  fixture.code32(4, {0xe280'1010U}); // add r1, r0, #16
  cpu = Fixture::task(false);
  result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[1] == 17 && cpu.registers[2] == 20);

  // Only the last byte of the word changes (the condition field): the
  // instruction becomes a conditional one that does not execute.
  fixture.code_byte(11, std::byte{0x02}); // addeq r2, r1, #3
  cpu = Fixture::task(false);
  cpu.registers[2] = 0x77;
  result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[2] == 0x77);
}

} // namespace

/* An entry whose first fetch is unmapped faults at the PC with an execute
 * access and retires nothing, in either instruction set. */
void unmapped_entry() {
  Fixture fixture{0x1a32'0c05};
  for (const bool thumb : {false, true}) {
    auto cpu = Fixture::task(thumb);
    cpu.pc = thumb ? key::t32_key(0x0070'0000U) : key::a32_key(0x0070'0000U);
    const auto before = cpu.pc;
    const auto result = fixture.run(cpu);
    require(result.reason == ExitReason::memory_fault);
    require(result.fault.address == 0x0070'0000U);
    require(result.fault.access == ::uml::memory::MemoryAccess::execute);
    require(result.retired_instructions == 0);
    require(cpu.pc == before);
  }
}

/* A step over BX into Thumb state and one over BX back leave the PC key of
 * the target, tag included, and the next step runs there. */
void single_step_over_interworking_branches() {
  Fixture fixture{0x1a32'0c06};
  fixture.code32(0, {0xe28f'2004U,  // add r2, pc, #4
                     0xe382'2001U,  // orr r2, r2, #1
                     0xe12f'ff12U}); // bx r2
  fixture.code16(12, {0x2007,       // movs r0, #7
                      0xdf00});     // svc #0
  auto cpu = Fixture::task(false);
  for (unsigned step = 0; step < 3; ++step) {
    const auto result = fixture.run(cpu, 1, true);
    require(result.reason == ExitReason::single_step);
    require(result.retired_instructions == 1);
  }
  require(cpu.registers[2] == code_base + 12 + 1);
  require(cpu.pc == key::t32_key(code_base + 12));
  auto result = fixture.run(cpu, 1, true);
  require(result.reason == ExitReason::single_step);
  require(cpu.registers[0] == 7);
  require(cpu.pc == key::t32_key(code_base + 14));
  result = fixture.run(cpu, 1, true);
  require(result.reason == ExitReason::system_call);
  require(cpu.pc == key::t32_key(code_base + 16));

  // BX the other way: Thumb to A32, one step.
  fixture.code16(32, {0x4778,   // bx pc (to the A32 word after the nop)
                      0xbf00});
  fixture.code32(36, {0xe3a0'1009U}); // mov r1, #9
  cpu = Fixture::task(true, 32);
  auto step = fixture.run(cpu, 1, true);
  require(step.reason == ExitReason::single_step);
  require(cpu.pc == key::a32_key(code_base + 36));
  step = fixture.run(cpu, 1, true);
  require(step.reason == ExitReason::single_step);
  require(cpu.registers[1] == 9);
}

/* The optional extensions of the complete level, by the A64 host feature
 * each is shown for. */
enum class Extension {
  crc32, aes, sha1, sha2, fp16, dotprod, fhm, bf16, i8mm, sb, rdm, fcma, jscvt,
};

/* One instruction of an extension, as an A32 word and as the T32 word
 * (halfword one in the upper half). */
struct ExtensionForm {
  const char *name;
  Extension extension;
  std::uint32_t a32;
  std::uint32_t t32;
};

constexpr std::array extension_forms{
    ExtensionForm{"CRC32W", Extension::crc32, 0xe1410042U, 0xfac1f0a2U},
    ExtensionForm{"AESE", Extension::aes, 0xf3b00302U, 0xffb00302U},
    ExtensionForm{"SHA1C", Extension::sha1, 0xf2020c44U, 0xef020c44U},
    ExtensionForm{"SHA256H", Extension::sha2, 0xf3020c44U, 0xff020c44U},
    ExtensionForm{"VADD.F16", Extension::fp16, 0xf2110d02U, 0xef110d02U},
    ExtensionForm{"VSDOT", Extension::dotprod, 0xfc210d02U, 0xfc210d02U},
    ExtensionForm{"VFMAL", Extension::fhm, 0xfc200891U, 0xfc200891U},
    ExtensionForm{"VDOT.BF16", Extension::bf16, 0xfc010d02U, 0xfc010d02U},
    ExtensionForm{"VUSDOT", Extension::i8mm, 0xfca10d02U, 0xfca10d02U},
    ExtensionForm{"SB", Extension::sb, 0xf57ff070U, 0xf3bf8f70U},
    ExtensionForm{"VQRDMLAH", Extension::rdm, 0xf3110b12U, 0xff110b12U},
    ExtensionForm{"VCADD", Extension::fcma, 0xfc910802U, 0xfc910802U},
    ExtensionForm{"VJCVT", Extension::jscvt, 0xeeb90bc1U, 0xeeb90bc1U},
};
/* VMAXNM Dd, Dn, Dm: the Armv8 floating-point additions, which every host
 * has and the host features do not gate. */
constexpr ExtensionForm always_present{"VMAXNM", Extension::crc32, 0xf3010f12U,
                                       0xff010f12U}; // its extension unused

/* The host as probed with every extension on, less `lacking` when it is
 * given: the A64 features a32_features reads. */
HostCapabilities extension_host(std::optional<Extension> lacking) {
  HostCapabilities host = ::uml::cpu::host_capabilities();
  host.advanced_simd = true;
  host.floating_point = true;
  const auto feature = [&host, lacking](Extension extension, ArmFeature which,
                                        bool *flag = nullptr) {
    const bool on = lacking != extension;
    if (flag != nullptr)
      *flag = on;
    if (on)
      host.supported.set(static_cast<std::size_t>(which));
    else
      host.supported.reset(static_cast<std::size_t>(which));
  };
  feature(Extension::crc32, ArmFeature::crc32, &host.crc32);
  feature(Extension::aes, ArmFeature::aes, &host.aes);
  feature(Extension::fp16, ArmFeature::fp16, &host.fp16);
  host.pmull = true;
  host.sha1 = lacking != Extension::sha1;
  host.sha2 = lacking != Extension::sha2;
  feature(Extension::dotprod, ArmFeature::dotprod);
  feature(Extension::fhm, ArmFeature::fhm);
  feature(Extension::bf16, ArmFeature::bf16);
  feature(Extension::i8mm, ArmFeature::i8mm);
  feature(Extension::sb, ArmFeature::sb);
  feature(Extension::rdm, ArmFeature::rdm);
  feature(Extension::fcma, ArmFeature::fcma);
  feature(Extension::jscvt, ArmFeature::jscvt);
  return host;
}

/* Runs each form, A32 and T32, at its own offset followed by an SVC, and
 * says whether it reached the SVC (true) or the illegal-instruction exit
 * (false). */
bool extension_form_runs(Fixture &fixture, std::size_t slot,
                         const ExtensionForm &form, bool thumb) {
  const std::size_t offset = slot * 16;
  if (thumb)
    fixture.code16(offset, {static_cast<std::uint16_t>(form.t32 >> 16),
                            static_cast<std::uint16_t>(form.t32), 0xdf00});
  else
    fixture.code32(offset, {form.a32, 0xef000000U});
  auto cpu = Fixture::task(thumb, offset);
  const auto result = fixture.run(cpu);
  if (result.reason == ExitReason::system_call)
    return true;
  if (result.reason != ExitReason::illegal_instruction) {
    std::fprintf(stderr, "%s %s: unexpected exit %d\n", form.name,
                 thumb ? "T32" : "A32", static_cast<int>(result.reason));
    require(false);
  }
  return false;
}

/* An extension decodes only where the host has its A64 feature, which is
 * what the guest is shown: on a host without it the form is UNDEFINED, not a
 * form the semantic library runs, and nothing else changes. */
void extensions_decode_only_where_the_host_has_them() {
  unsigned long generation = 0x1a32'0d00;
  {
    const auto host = extension_host(std::nullopt);
    const ::uml::cpu::HostCapabilitiesScope scope{host};
    Fixture fixture{generation++};
    std::size_t slot = 0;
    for (const auto &form : extension_forms)
      for (const bool thumb : {false, true}) {
        if (!extension_form_runs(fixture, slot++, form, thumb)) {
          std::fprintf(stderr, "%s %s is UNDEFINED on a host with it\n",
                       form.name, thumb ? "T32" : "A32");
          require(false);
        }
      }
  }
  for (const auto &lacking : extension_forms) {
    const auto host = extension_host(lacking.extension);
    const ::uml::cpu::HostCapabilitiesScope scope{host};
    Fixture fixture{generation++};
    std::size_t slot = 0;
    for (const auto &form : extension_forms)
      for (const bool thumb : {false, true}) {
        const bool runs = extension_form_runs(fixture, slot++, form, thumb);
        if (runs != (form.extension != lacking.extension)) {
          std::fprintf(stderr, "%s %s %s on a host without %s\n", form.name,
                       thumb ? "T32" : "A32", runs ? "runs" : "is UNDEFINED",
                       lacking.name);
          require(false);
        }
      }
    for (const bool thumb : {false, true})
      require(extension_form_runs(fixture, slot++, always_present, thumb));
  }
}

// ---------------------------------------------------------------------------
// A block goes on past a conditional forward branch (B with a condition, CBZ,
// CBNZ) and leaves by a side exit when it is taken. Random programs of those
// branches among data processing, run as the translator forms them and one
// instruction at a time (which never goes on past a branch), must end in the
// same registers, flags and count.

class Lcg final {
public:
  explicit Lcg(std::uint64_t seed) : state_{seed | 1U} {}
  std::uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }
  unsigned below(unsigned bound) { return static_cast<unsigned>(next() % bound); }

private:
  std::uint64_t state_;
};

struct Outcome final {
  arm64::CpuState cpu;
  std::size_t retired;
  ::uml::cpu::ExitReason reason;
};

Outcome run_whole(Fixture &fixture, arm64::CpuState cpu) {
  const auto result = fixture.run(cpu, 4096);
  return {cpu, result.retired_instructions, result.reason};
}

Outcome run_stepped(Fixture &fixture, arm64::CpuState cpu) {
  std::size_t retired = 0;
  for (unsigned step = 0; step < 4096; ++step) {
    const auto result = fixture.run(cpu, 1, true);
    retired += result.retired_instructions;
    if (result.reason != ExitReason::single_step)
      return {cpu, retired, result.reason};
  }
  return {cpu, retired, ExitReason::single_step};
}

void require_same(const Outcome &whole, const Outcome &stepped,
                  const char *what, unsigned program) {
  bool same = whole.reason == stepped.reason &&
              whole.retired == stepped.retired &&
              whole.cpu.pc == stepped.cpu.pc &&
              (whole.cpu.nzcv & 0xf000'0000U) == (stepped.cpu.nzcv & 0xf000'0000U);
  for (unsigned i = 0; i < 15; ++i)
    same = same && whole.cpu.registers[i] == stepped.cpu.registers[i];
  if (!same) {
    std::fprintf(stderr, "%s program %u: whole retired=%zu pc=%llx nzcv=%08x "
                         "stepped retired=%zu pc=%llx nzcv=%08x\n",
                 what, program, whole.retired,
                 static_cast<unsigned long long>(whole.cpu.pc),
                 static_cast<unsigned>(whole.cpu.nzcv), stepped.retired,
                 static_cast<unsigned long long>(stepped.cpu.pc),
                 static_cast<unsigned>(stepped.cpu.nzcv));
    for (unsigned i = 0; i < 15; ++i)
      std::fprintf(stderr, "  r%u whole=%08x stepped=%08x\n", i,
                   static_cast<unsigned>(whole.cpu.registers[i]),
                   static_cast<unsigned>(stepped.cpu.registers[i]));
  }
  require(same);
}

arm64::CpuState random_registers(Lcg &random, bool thumb) {
  auto cpu = Fixture::task(thumb);
  for (unsigned i = 0; i < 13; ++i) {
    static constexpr std::uint32_t special[] = {0, 1, 2, 0x7fff'ffffU,
                                                0x8000'0000U, 0xffff'ffffU};
    cpu.registers[i] = random.below(3) == 0
                           ? special[random.below(std::size(special))]
                           : static_cast<std::uint32_t>(random.next());
  }
  cpu.nzcv = static_cast<std::uint32_t>(random.next()) & 0xf000'0000U;
  return cpu;
}

void side_exit_branches() {
  constexpr unsigned programs = 120;
  Fixture fixture{0x1a32'0c0a, 24};
  Lcg random{0x51de'e817};
  for (const bool thumb : {false, true}) {
    for (unsigned p = 0; p < programs; ++p) {
      // A third of the programs are longer than a block holds.
      const unsigned length = p % 3 == 0 ? 90 + random.below(30) : 12 + random.below(40);
      const std::size_t unit = thumb ? 2 : 4;
      const std::size_t offset = static_cast<std::size_t>(p % 40) * 512;
      std::vector<std::uint32_t> words(length);
      std::vector<std::uint16_t> halves(length);
      for (unsigned i = 0; i < length; ++i) {
        const unsigned rd = random.below(12), rn = random.below(12);
        const bool branch = random.below(4) == 0 && i + 2 < length;
        // A forward branch skips 1 to 4 instructions, or all that remain.
        const unsigned skip = std::min<unsigned>(1 + random.below(4), length - i - 1);
        if (!thumb) {
          static constexpr unsigned ops[] = {0, 1, 2, 3, 4, 12, 13, 14};
          const unsigned op = ops[random.below(std::size(ops))];
          const unsigned cond = random.below(10) == 0 ? random.below(15)
                                                      : 14;
          if (branch) {
            // The target is skip + 1 instructions on: PC is two ahead.
            const unsigned bcond = random.below(8) == 0 ? 14 : random.below(14);
            words[i] = bcond << 28 | 0x0a00'0000U | (skip - 1U);
          } else {
            const bool s = random.below(2) != 0;
            // MOV's Rn field is zero.
            words[i] = cond << 28 | 0x0200'0000U | op << 21 |
                       (s ? 1U << 20 : 0U) | (op == 13U ? 0U : rn) << 16 |
                       rd << 12 | random.below(256);
          }
        } else {
          const unsigned lo = random.below(8), lo2 = random.below(8),
                         lo3 = random.below(8);
          if (branch && random.below(3) == 0) {
            // CBZ/CBNZ Rn, target: PC is two halfwords ahead, imm6 = skip - 1.
            const unsigned imm6 = skip - 1U;
            halves[i] = static_cast<std::uint16_t>(
                0xb100U | (random.below(2) != 0 ? 0x0800U : 0U) |
                (imm6 >> 5 & 1U) << 9 | (imm6 & 31U) << 3 | lo);
          } else if (branch) {
            halves[i] = static_cast<std::uint16_t>(
                0xd000U | random.below(14) << 8 | (skip - 1U));
          } else {
            switch (random.below(8)) {
            case 0: halves[i] = static_cast<std::uint16_t>(0x2000U | lo << 8 | random.below(256)); break;
            case 1: halves[i] = static_cast<std::uint16_t>(0x1c00U | random.below(8) << 6 | lo2 << 3 | lo); break;
            case 2: halves[i] = static_cast<std::uint16_t>(0x1a00U | lo3 << 6 | lo2 << 3 | lo); break;
            case 3: halves[i] = static_cast<std::uint16_t>(0x1800U | lo3 << 6 | lo2 << 3 | lo); break;
            case 4: halves[i] = static_cast<std::uint16_t>(0x2800U | lo << 8 | random.below(256)); break;
            case 5: halves[i] = static_cast<std::uint16_t>(0x4000U | lo2 << 3 | lo); break;
            case 6: halves[i] = static_cast<std::uint16_t>(0x4040U | lo2 << 3 | lo); break;
            default: halves[i] = static_cast<std::uint16_t>(random.below(32) << 6 | lo2 << 3 | lo); break;
            }
          }
        }
      }
      for (unsigned i = 0; i < length; ++i) {
        if (thumb)
          fixture.code16(offset + i * unit, {halves[i]});
        else
          fixture.code32(offset + i * unit, {words[i]});
      }
      if (thumb)
        fixture.code16(offset + length * unit, {0xdf00U});
      else
        fixture.code32(offset + length * unit, {0xef00'0000U});
      for (unsigned round = 0; round < 6; ++round) {
        auto cpu = random_registers(random, thumb);
        cpu.pc = thumb ? key::t32_key(code_base + static_cast<std::uint32_t>(offset))
                       : key::a32_key(code_base + static_cast<std::uint32_t>(offset));
        const auto whole = run_whole(fixture, cpu);
        require(whole.reason == ExitReason::system_call);
        const auto stepped = run_stepped(fixture, cpu);
        require_same(whole, stepped, thumb ? "t32" : "a32", p);
      }
    }
  }
}

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  unmapped_entry();
  it_blocks_whole_stepped_and_entered_mid_block();
  single_step_over_interworking_branches();
  long_blocks_and_the_budget();
  static_exits_chain();
  rewritten_t32_halfwords();
  rewritten_a32_words();
  side_exit_branches();
  extensions_decode_only_where_the_host_has_them();
  std::puts("arm64-jit-a32-control: ok");
  return 0;
}
