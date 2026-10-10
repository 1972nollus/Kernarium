// SPDX-License-Identifier: GPL-2.0-or-later
/* A32 data processing on the JIT against the reference
 * (arm64_jit_a32_reference.hpp): random sequences of instructions under
 * random conditions and flags. The corpus lane carries T32, and the control
 * test the IT blocks. */
#include "arm64_jit_a32_reference.hpp"
#include "arm64_jit_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace uml::emulator::tests;
using namespace uml::emulator::tests::a32;
using ::uml::cpu::ExitReason;

namespace {

class Random final {
public:
  explicit Random(std::uint64_t seed) : state_{seed | 1U} {}
  std::uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }
  unsigned below(unsigned bound) { return static_cast<unsigned>(next() % bound); }
  std::uint32_t value() {
    static constexpr std::uint32_t special[] = {
        0, 1, 2, 0x7fff'ffffU, 0x8000'0000U, 0xffff'ffffU, 0x8000'0001U,
        0xff, 0x100, 0xffff, 0x1'0000U, 0x7fff'ffffU - 1U, 31, 32, 33, 255};
    switch (below(4)) {
    case 0: return special[below(std::size(special))];
    case 1: return static_cast<std::uint32_t>(next()) >> below(32);
    default: return static_cast<std::uint32_t>(next());
    }
  }

private:
  std::uint64_t state_;
};

struct Instruction final {
  std::uint32_t word;
  bool thumb32;
};

// ---------------------------------------------------------------------------
// A32.

std::uint32_t a32_dp_imm(unsigned cond, ref::DpOp op, bool s, unsigned rd,
                         unsigned rn, unsigned imm12) {
  return cond << 28 | 1U << 25 | static_cast<unsigned>(op) << 21 | (s ? 1U << 20 : 0U) |
         rn << 16 | rd << 12 | imm12;
}
std::uint32_t a32_dp_reg(unsigned cond, ref::DpOp op, bool s, unsigned rd,
                         unsigned rn, unsigned rm, unsigned type,
                         unsigned imm5) {
  return cond << 28 | static_cast<unsigned>(op) << 21 | (s ? 1U << 20 : 0U) |
         rn << 16 | rd << 12 | imm5 << 7 | type << 5 | rm;
}
std::uint32_t a32_dp_rsr(unsigned cond, ref::DpOp op, bool s, unsigned rd,
                         unsigned rn, unsigned rm, unsigned type,
                         unsigned rs) {
  return cond << 28 | static_cast<unsigned>(op) << 21 | (s ? 1U << 20 : 0U) |
         rn << 16 | rd << 12 | rs << 8 | type << 5 | 1U << 4 | rm;
}

struct Machine final {
  ref::State s;
};

void a32_reference_step(ref::State &s, std::uint32_t word) {
  const unsigned cond = word >> 28;
  if (!condition_passed(s, cond))
    return;
  const auto op = static_cast<ref::DpOp>(word >> 21 & 15U);
  const bool set_flags = (word >> 20 & 1U) != 0U;
  const unsigned rn = word >> 16 & 15U;
  const unsigned rd = word >> 12 & 15U;
  ref::Shifted operand2;
  if ((word >> 25 & 1U) != 0U) {
    operand2 = ref::arm_expand_imm_c(word & 0xfffU, s.c);
  } else {
    const unsigned rm = word & 15U;
    const unsigned type = word >> 5 & 3U;
    if ((word >> 4 & 1U) != 0U) {
      const unsigned amount = s.r[word >> 8 & 15U] & 0xffU;
      operand2 = ref::shift_c(s.r[rm], static_cast<ref::Shift>(type), amount, s.c);
    } else {
      unsigned amount = word >> 7 & 31U;
      auto shift = static_cast<ref::Shift>(type);
      if ((type == 1 || type == 2) && amount == 0)
        amount = 32;
      if (type == 3 && amount == 0)
        shift = ref::Shift::rrx;
      operand2 = ref::shift_c(s.r[rm], shift, amount, s.c);
    }
  }
  const auto result =
      ref::data_processing(s, op, s.r[rn], operand2.value, operand2.carry, set_flags);
  if (!ref::dp_tests_only(op))
    s.r[rd] = result;
}

[[nodiscard]] bool state_matches(const arm64::CpuState &cpu,
                                 const ref::State &expected) {
  bool ok = (cpu.nzcv & 0xf000'0000U) == expected.nzcv();
  for (unsigned i = 0; i < 15; ++i)
    ok = ok && cpu.registers[i] == expected.r[i];
  return ok;
}

void report_state(const arm64::CpuState &cpu, const ref::State &expected,
                  const std::vector<std::uint32_t> &program,
                  std::size_t executed, const ref::State &initial,
                  const char *what) {
  std::fprintf(stderr, "MISMATCH (%s) after %zu instructions\n", what, executed);
  std::fprintf(stderr, "  program:");
  for (std::size_t i = 0; i < program.size(); ++i)
    std::fprintf(stderr, " %08x", program[i]);
  std::fprintf(stderr, "\n  initial nzcv=%08x:", initial.nzcv());
  for (unsigned i = 0; i < 15; ++i)
    std::fprintf(stderr, " r%u=%08x", i, initial.r[i]);
  std::fprintf(stderr, "\n  expected nzcv=%08x:", expected.nzcv());
  for (unsigned i = 0; i < 15; ++i)
    std::fprintf(stderr, " r%u=%08x", i, expected.r[i]);
  std::fprintf(stderr, "\n  actual   nzcv=%08x:", cpu.nzcv & 0xf000'0000U);
  for (unsigned i = 0; i < 15; ++i)
    std::fprintf(stderr, " r%u=%08x", i, static_cast<std::uint32_t>(cpu.registers[i]));
  std::fprintf(stderr, "\n");
}

void load_state(arm64::CpuState &cpu, const ref::State &s) {
  for (unsigned i = 0; i < 15; ++i)
    cpu.registers[i] = s.r[i];
  cpu.nzcv = s.nzcv();
}

ref::State random_state(Random &random) {
  ref::State s;
  for (unsigned i = 0; i < 15; ++i)
    s.r[i] = random.value();
  s.set_nzcv(static_cast<std::uint32_t>(random.next()));
  return s;
}

Instruction random_a32_dp(Random &random) {
  const unsigned cond = random.below(8) == 0 ? random.below(15) : 14;
  const auto op = static_cast<ref::DpOp>(random.below(16));
  const bool tests = ref::dp_tests_only(op);
  const bool s = tests || random.below(2) != 0;
  const unsigned rd = tests ? 0 : random.below(15);
  const bool moves = op == ref::DpOp::mov || op == ref::DpOp::mvn;
  const unsigned rn = moves ? 0 : random.below(15);
  switch (random.below(3)) {
  case 0:
    return {a32_dp_imm(cond, op, s, rd, rn, random.below(4096)), false};
  case 1:
    return {a32_dp_reg(cond, op, s, rd, rn, random.below(15), random.below(4),
                       random.below(32)),
            false};
  default:
    return {a32_dp_rsr(cond, op, s, rd, rn, random.below(15), random.below(4),
                       random.below(15)),
            false};
  }
}

/* Runs `words` followed by an SVC at `offset` from `initial`, and returns the
 * task after it. */
arm64::CpuState run_sequence(Fixture &fixture, std::size_t offset,
                             const std::vector<std::uint32_t> &words,
                             const ref::State &initial) {
  for (std::size_t i = 0; i < words.size(); ++i)
    fixture.code32(offset + 4 * i, {words[i]});
  fixture.code32(offset + 4 * words.size(), {0xef000000U}); // svc #0
  auto cpu = Fixture::task(false, offset);
  load_state(cpu, initial);
  const auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(result.retired_instructions == words.size() + 1U);
  require(cpu.pc == key::a32_key(code_base + static_cast<std::uint32_t>(
                                                 offset + 4 * (words.size() + 1U))));
  return cpu;
}

ref::State reference_run(const std::vector<std::uint32_t> &words,
                         const ref::State &initial) {
  ref::State expected = initial;
  for (const auto word : words)
    a32_reference_step(expected, word);
  return expected;
}

/* The first prefix of a failing sequence that still fails, with the state
 * before its last instruction. */
[[noreturn]] void bisect(Fixture &fixture, std::size_t scratch,
                         const std::vector<std::uint32_t> &words,
                         const ref::State &initial) {
  for (std::size_t k = 1; k <= words.size(); ++k) {
    const std::vector<std::uint32_t> prefix(words.begin(), words.begin() + k);
    const auto expected = reference_run(prefix, initial);
    const auto cpu = run_sequence(fixture, scratch + k * 1024, prefix, initial);
    if (!state_matches(cpu, expected)) {
      std::fprintf(stderr, "first failing instruction: #%zu = %08x\n", k - 1,
                   words[k - 1]);
      report_state(cpu, expected, prefix, k, initial, "prefix");
      break;
    }
  }
  std::abort();
}

void a32_random_sequences(unsigned long generation, std::uint64_t seed,
                          unsigned programs, unsigned length) {
  constexpr std::size_t stride = 1024;
  const std::size_t program_bytes = static_cast<std::size_t>(programs) * stride;
  const std::size_t scratch = (program_bytes + guest_page - 1) / guest_page * guest_page;
  const std::size_t pages = scratch / guest_page + (length + 2) * stride / guest_page + 1;
  Fixture fixture{generation, pages};
  Random random{seed};
  for (unsigned p = 0; p < programs; ++p) {
    std::vector<std::uint32_t> program;
    for (unsigned i = 0; i < length; ++i)
      program.push_back(random_a32_dp(random).word);
    const auto initial = random_state(random);
    const auto expected = reference_run(program, initial);
    const auto cpu = run_sequence(fixture, static_cast<std::size_t>(p) * stride,
                                  program, initial);
    if (!state_matches(cpu, expected)) {
      report_state(cpu, expected, program, length, initial, "a32 random");
      bisect(fixture, scratch, program, initial);
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  unsigned programs = 600;
  if (argc > 1)
    programs = static_cast<unsigned>(std::atoi(argv[1]));
  a32_random_sequences(0x1a32'0100, 0x1234'5678'9abcULL, programs, 24);
  std::puts("arm64-jit-a32-data-processing: ok");
  return 0;
}
