// SPDX-License-Identifier: GPL-2.0-or-later
/* The JIT's AArch32 control behaviour: IT blocks entered from the start, one
 * instruction at a time and from the middle; blocks longer than the compiler
 * keeps in one translation and the instruction budget across them; and
 * rewritten code (A32 words, T32 halfwords of a 32-bit instruction, one byte)
 * seen by the translations that held the old code. */
#include "arm64_jit_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include <array>
#include <cstdio>
#include <vector>

using namespace uml::emulator::tests;
using namespace uml::emulator::tests::a32;
using ::uml::cpu::ExitReason;

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
  Fixture fixture{0x1a32'0c02};
  constexpr std::size_t adds = 300;
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
    require(++exits < 100);
  }
  require(exits >= 2);
  require(retired == adds + 1);
  require(cpu.registers[0] == adds);
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

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  it_blocks_whole_stepped_and_entered_mid_block();
  long_blocks_and_the_budget();
  rewritten_t32_halfwords();
  rewritten_a32_words();
  std::puts("arm64-jit-a32-control: ok");
  return 0;
}
