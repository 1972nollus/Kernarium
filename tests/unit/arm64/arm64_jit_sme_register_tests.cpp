// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The JIT's register-only SVE and SME forms at run time (S4 milestone M2).
 * They run natively in SM=1 translations, so every test needs a host whose
 * profile implements SME (an SME host with DARWIN_UML_GUEST_SME=1) and is
 * skipped elsewhere; arm64-jit-sme-codegen checks the translations on any
 * host. The tests check what the JIT adds around a guest word: general
 * register fields re-encoded (resident or not, SP, XZR), the ZA slice index
 * in W12, the guest's NZCV around flag-setting forms, the guest's FPCR in
 * the host's, the guest's FPSR accumulated in CpuState exactly, even when
 * kernel entries reset the host's, and MOVPRFX as a move. The corpus
 * checks the forms themselves against the M4.
 */
#include "arm64_jit_sme_test_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/jit/jit_sme_support.hpp"
#include "cpu/arm64/sme_profile.hpp"

#include <mach/mach.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

using namespace uml::emulator::tests;
using namespace uml::emulator::tests::jit_sme;

namespace {

constexpr std::uint32_t mrs_nzcv(unsigned t) noexcept {
  return 0xd53b'4200U | t;
}
constexpr std::uint32_t mrs_fpsr(unsigned t) noexcept {
  return 0xd53b'4420U | t;
}
constexpr std::uint32_t msr_fpcr(unsigned t) noexcept {
  return 0xd51b'4400U | t;
}
constexpr std::uint32_t msr_fpsr_zero = 0xd51b'443fU;
/* B.NE by `words` (negative: backwards). */
constexpr std::uint32_t branch_not_equal(int words) noexcept {
  return 0x5400'0001U |
         ((static_cast<std::uint32_t>(words) & 0x7'ffffU) << 5);
}

constexpr std::uint32_t ptrue_p0_d = 0x25d8'e3e0U;
constexpr std::uint32_t ptrue_p0_s = 0x2598'e3e0U;
constexpr std::uint32_t fmov_z1_one = 0x25b9'ce01U; // fmov z1.s, #1.0
constexpr float one = 1.0F;

[[nodiscard]] std::uint64_t lane64(const std::vector<std::byte> &file,
                                   unsigned svl, unsigned z, unsigned lane) {
  std::uint64_t value;
  std::memcpy(&value, file.data() + arm64::sme_z_offset(svl, z) + lane * 8U,
              sizeof value);
  return value;
}

[[nodiscard]] std::uint32_t lane32(const std::vector<std::byte> &file,
                                   unsigned svl, unsigned z, unsigned lane) {
  std::uint32_t value;
  std::memcpy(&value, file.data() + arm64::sme_z_offset(svl, z) + lane * 4U,
              sizeof value);
  return value;
}

/* Every 32-bit lane of Z_z holds `value`. */
[[nodiscard]] bool lanes_are(const std::vector<std::byte> &file, unsigned svl,
                             unsigned z, std::uint32_t value) {
  for (unsigned lane = 0; lane < svl / 4U; ++lane)
    if (lane32(file, svl, z, lane) != value) {
      std::fprintf(stderr, "z%u.s[%u] = %#x, expected %#x\n", z, lane,
                   lane32(file, svl, z, lane), value);
      return false;
    }
  return true;
}

[[nodiscard]] std::uint64_t host_fpcr() noexcept {
  std::uint64_t value;
  __asm__ volatile("mrs %0, fpcr" : "=r"(value));
  return value;
}

/* General-register fields: destinations, sources, read-modify-write, SP
 * and XZR, the first seven in resident host registers and the rest in
 * fallback scratch. */
void general_registers_reach_their_fields() {
  Machine machine{9501, 0x210'0000, arm64::svcr_sm};
  machine.code(0, {ptrue_p0_d,
                   0x0420'e3e3U, // cntb x3
                   0x05e0'3881U, // mov z1.d, x4
                   0x05e0'a025U, // lasta x5, p0, z1.d
                   0x05e0'3be2U, // mov z2.d, sp
                   0x05e1'a046U, // lastb x6, p0, z2.d
                   0x043f'57ffU, // addvl sp, sp, #-1
                   0x043f'5047U, // addvl x7, sp, #2
                   0x05e4'3be3U, // insr z3.d, xzr
                   0x05e0'a069U, // lasta x9, p0, z3.d
                   0x0430'e3f4U, // incb x20
                   0x25ec'8815U, // incp x21, p0.d
                   0x25ea'8816U, // sqdecp x22, p0.d, w22
                   0x04f8'4ee4U, // index z4.d, x23, x24
                   0x05e1'a099U, // lastb x25, p0, z4.d
                   0x05f1'a09aU, // clastb x26, p0, x26, z4.d
                   0x04bf'507bU, // rdvl x27, #3
                   system_call});
  auto &cpu = machine.cpu;
  const std::uint64_t sp = cpu.sp;
  cpu.registers[4] = 0x1122'3344'5566'7788ULL;
  cpu.registers[20] = 1000;
  cpu.registers[21] = 5;
  cpu.registers[22] = 0xffff'ffff'8000'0003ULL;
  cpu.registers[23] = 100;
  cpu.registers[24] = 7;
  cpu.registers[26] = 0xdead;
  std::size_t retired = 0;
  require(machine.run(retired) == Event::system_call);
  require(retired == 18U);
  const std::uint64_t svl = machine.svl();
  const std::uint64_t lanes = svl / 8U;
  require(cpu.registers[3] == svl);
  require(cpu.registers[5] == 0x1122'3344'5566'7788ULL);
  require(cpu.registers[6] == sp);
  require(cpu.sp == sp - svl);
  require(cpu.registers[7] == sp - svl + 2U * svl);
  require(cpu.registers[9] == 0U);
  require(cpu.registers[20] == 1000U + svl);
  require(cpu.registers[21] == 5U + lanes);
  const auto decremented = std::max<std::int64_t>(
      std::numeric_limits<std::int32_t>::min(),
      static_cast<std::int64_t>(static_cast<std::int32_t>(0x8000'0003U)) -
          static_cast<std::int64_t>(lanes));
  require(cpu.registers[22] == static_cast<std::uint64_t>(decremented));
  require(cpu.registers[25] == 100U + (lanes - 1U) * 7U);
  require(cpu.registers[26] == cpu.registers[25]);
  require(cpu.registers[27] == 3U * svl);
  machine.require_preserved(
      {3, 5, 6, 7, 9, 20, 21, 22, 25, 26, 27, stack_pointer_index});
}

/* Flag-setting forms take the guest's NZCV and leave theirs for the next
 * reader, a base instruction or another SVE form. */
void flags_follow_the_guest() {
  Machine machine{9502, 0x220'0000, arm64::svcr_sm};
  machine.code(0, {ptrue_p0_d,
                   0x25eb'1d41U, // whilelo p1.d, x10, x11
                   mrs_nzcv(12),
                   0x9a9f'57f0U, // cset x16, mi
                   0x2550'c020U, // ptest p0, p1.b
                   mrs_nzcv(13),
                   0x25ee'21d0U, // ctermne x14, x14
                   mrs_nzcv(17),
                   0x25f8'c001U, // mov z1.d, #0
                   0x25c0'8022U, // cmpeq p2.d, p0/z, z1.d, #0
                   mrs_nzcv(18), system_call});
  auto &cpu = machine.cpu;
  cpu.registers[10] = 0;
  cpu.registers[11] = 1;
  cpu.registers[14] = 42;
  std::size_t retired = 0;
  require(machine.run(retired) == Event::system_call);
  /* One active element of at least two: N, not Z, C (the last is not
   * active). */
  require(cpu.registers[12] == 0xa000'0000U);
  require(cpu.registers[16] == 1U);
  require(cpu.registers[13] == 0xa000'0000U);
  /* CTERMNE of equal values: N and V clear, V from NOT C; Z and C kept. */
  require(cpu.registers[17] == 0x2000'0000U);
  /* Every element equal: N, not Z, not C. */
  require(cpu.registers[18] == 0x8000'0000U);
  require(cpu.nzcv == 0x8000'0000U);
  machine.require_preserved({12, 13, 16, 17, 18}, true);
}

/* ZA arithmetic and MOVA, with slice indexes in guest W12-W15, resident or
 * not. */
void za_forms_take_their_slice_indexes() {
  Machine machine{9503, 0x230'0000, arm64::svcr_mask};
  machine.code(0, {zero_za(0xff), ptrue_p0_s, fmov_z1_one,
                   0x25b9'c002U, // fmov z2.s, #2.0
                   0x8082'0020U, // fmopa za0.s, p0/m, p0/m, z1.s, z2.s
                   0x04a1'4004U, // index z4.s, #0, #1
                   0xc091'0081U, // addva za1.s, p0/m, p0/m, z4.s
                   0xc082'20a5U, // mova z5.s, p0/m, za1h.s[w13, 1]
                   0x25b8'c9a6U, // mov z6.s, #77
                   0xc080'00c4U, // mova za1h.s[w12, 0], p0/m, z6.s
                   0xc082'4087U, // mova z7.s, p0/m, za1h.s[w14, 0]
                   0xc082'6008U, // mova z8.s, p0/m, za0h.s[w15, 0]
                   system_call});
  auto &cpu = machine.cpu;
  cpu.registers[12] = 5;
  cpu.registers[13] = 2;
  cpu.registers[14] = 5;
  cpu.registers[15] = 0;
  std::size_t retired = 0;
  require(machine.run(retired) == Event::system_call);
  const auto file = machine.file();
  const unsigned svl = machine.svl();
  require(lanes_are(file, svl, 5, 3U));
  require(lanes_are(file, svl, 7, 77U));
  require(lanes_are(file, svl, 8, std::bit_cast<std::uint32_t>(2.0F)));
  machine.require_preserved();
}

/* The guest's FPCR rounds and flushes SM=1 floating point, an MSR FPCR in
 * the block takes effect at once, and the host's FPCR is back after the
 * run. */
void fpcr_controls_streaming_floating_point() {
  Machine machine{9504, 0x240'0000, arm64::svcr_sm};
  machine.code(0, {fmov_z1_one,
                   0x05a0'3882U, // mov z2.s, w4
                   0x6582'0023U, // fadd z3.s, z1.s, z2.s
                   msr_fpcr(5),
                   0x6582'0024U, // fadd z4.s, z1.s, z2.s
                   mrs_fpsr(6), msr_fpcr(7),
                   0x6582'0025U, // fadd z5.s, z1.s, z2.s
                   mrs_fpsr(8), system_call});
  auto &cpu = machine.cpu;
  cpu.registers[4] = 1;                  // the smallest denormal
  cpu.registers[5] = 0x0080'0000U;       // RMode -inf
  cpu.registers[7] = 0x0140'0000U;       // FZ, RMode +inf
  const auto before = host_fpcr();
  std::size_t retired = 0;
  require(machine.run(retired) == Event::system_call);
  require(host_fpcr() == before);
  const auto file = machine.file();
  const unsigned svl = machine.svl();
  const auto bits = std::bit_cast<std::uint32_t>(one);
  /* RMode +inf (initial_fpcr), then -inf: inexact either way. */
  require(lanes_are(file, svl, 3, bits + 1U));
  require(lanes_are(file, svl, 4, bits));
  require(cpu.registers[6] == (initial_fpsr | 0x10U));
  /* FZ flushes the denormal input: exact, with IDC. */
  require(lanes_are(file, svl, 5, bits));
  require(cpu.registers[8] == (initial_fpsr | 0x90U));
  arm64::restore_native_fp_environment(cpu);
  require(cpu.effective_fpcr() == 0x0140'0000U);
}

/*
 * A long SM=1 loop that another thread keeps stopping in the kernel, where
 * XNU resets the host's FPSR to 0x0800009F. Every iteration clears the
 * guest's FPSR and checks that a NaN comparison raised exactly IOC, so a
 * flag lost or invented in one iteration counts; the comparison's
 * predicate is also its governing predicate, and a destructive FADD counts
 * the iterations, so a rerun that saw its own result would show.
 */
void fp_flags_survive_kernel_entries() {
  constexpr std::uint64_t iterations = 4'000'000;
  Machine machine{9505, 0x250'0000, arm64::svcr_sm};
  machine.code(0, {ptrue_p0_s,
                   0x25b8'c002U, // mov z2.s, #0
                   0x25b8'c00aU, // mov z10.s, #0
                   0x25b9'ce0bU, // fmov z11.s, #1.0
                   0x05a0'388cU, // mov z12.s, w4
                   0x25b9'ce0dU, // fmov z13.s, #1.0
                   /* loop: */
                   fmov_z1_one,
                   0x658d'8041U, // fdiv z1.s, p0/m, z1.s, z2.s
                   msr_fpsr_zero,
                   0x6580'816aU, // fadd z10.s, p0/m, z10.s, z11.s
                   0x2598'e3e1U, // ptrue p1.s
                   0x658d'4591U, // fcmgt p1.s, p1/z, z12.s, z13.s
                   mrs_fpsr(20),
                   0xf100'069fU, // cmp x20, #1
                   0x9a95'06b5U, // cinc x21, x21, ne
                   0xf100'0442U, // subs x2, x2, #1
                   branch_not_equal(-10), mrs_fpsr(3), system_call});
  auto &cpu = machine.cpu;
  cpu.registers[2] = iterations;
  cpu.registers[4] = 0x7fc0'0000U; // quiet NaN
  cpu.registers[21] = 0;
  const auto target = mach_thread_self();
  std::atomic<bool> running{true};
  std::atomic<unsigned> stops{0};
  std::thread interrupter{[&] {
    while (running.load(std::memory_order_relaxed)) {
      if (thread_suspend(target) == KERN_SUCCESS) {
        /* Reading the state waits until the thread has stopped. */
        arm_thread_state64_t state{};
        mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(target, ARM_THREAD_STATE64,
                             reinterpret_cast<thread_state_t>(&state),
                             &count) == KERN_SUCCESS)
          stops.fetch_add(1, std::memory_order_relaxed);
        thread_resume(target);
      }
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }};
  std::size_t retired = 0;
  const auto event = machine.run(retired, 1'000'000);
  running.store(false, std::memory_order_relaxed);
  interrupter.join();
  mach_port_deallocate(mach_task_self(), target);
  require(event == Event::system_call);
  std::fprintf(stderr, "FP loop stopped in the kernel %u times, %llu "
               "iterations miscounted their flags\n", stops.load(),
               static_cast<unsigned long long>(cpu.registers[21]));
  require(stops.load() >= 10U);
  require(cpu.registers[21] == 0U);
  require(cpu.registers[3] == 0x1U);
  require(machine.guest_fpsr() == 0x1U);
  const auto file = machine.file();
  const unsigned svl = machine.svl();
  require(lanes_are(file, svl, 1, 0x7f80'0000U)); // +inf
  require(lanes_are(file, svl, 10, std::bit_cast<std::uint32_t>(
                                       static_cast<float>(iterations))));
}

/* MOVPRFX in each form, with integer and floating-point consumers. */
void movprfx_is_a_move() {
  Machine machine{9506, 0x260'0000, arm64::svcr_sm};
  machine.code(0, {ptrue_p0_s,
                   0x04a1'4002U, // index z2.s, #0, #1
                   0x25b8'c143U, // mov z3.s, #10
                   0x2598'e041U, // ptrue p1.s, vl2
                   0x25b8'cc81U, // mov z1.s, #100
                   0x0420'bc41U, // movprfx z1, z2
                   0x0480'0061U, // add z1.s, p0/m, z1.s, z3.s
                   0x25b8'cc84U, // mov z4.s, #100
                   0x0491'2444U, // movprfx z4.s, p1/m, z2.s
                   0x0480'0464U, // add z4.s, p1/m, z4.s, z3.s
                   0x25b8'cc85U, // mov z5.s, #100
                   0x0490'2445U, // movprfx z5.s, p1/z, z2.s
                   0x0480'0465U, // add z5.s, p1/m, z5.s, z3.s
                   0x25b9'c008U, // fmov z8.s, #2.0
                   0x0420'bd06U, // movprfx z6, z8
                   0x6580'8106U, // fadd z6.s, p0/m, z6.s, z8.s
                   0x05a4'3bc9U, // insr z9.s, w30
                   0x05a4'3be9U, // insr z9.s, wzr
                   0x05a8'a7eaU, // mov z10.s, p1/m, wsp
                   system_call});
  auto &cpu = machine.cpu;
  cpu.registers[30] = 0x1234'5678'abcd'1234ULL;
  std::size_t retired = 0;
  require(machine.run(retired) == Event::system_call);
  const auto file = machine.file();
  const unsigned svl = machine.svl();
  for (unsigned lane = 0; lane < svl / 4U; ++lane) {
    require(lane32(file, svl, 1, lane) == lane + 10U);
    require(lane32(file, svl, 4, lane) == (lane < 2U ? lane + 10U : 100U));
    require(lane32(file, svl, 5, lane) == (lane < 2U ? lane + 10U : 0U));
  }
  require(lanes_are(file, svl, 6, std::bit_cast<std::uint32_t>(4.0F)));
  require(lane32(file, svl, 9, 0) == 0U);
  require(lane32(file, svl, 9, 1) == 0xabcd'1234U);
  require(lane32(file, svl, 10, 0) == static_cast<std::uint32_t>(cpu.sp));
  require(lane32(file, svl, 10, 1) == static_cast<std::uint32_t>(cpu.sp));
  require(lane64(file, svl, 9, 0) == 0xabcd'1234'0000'0000ULL);
  require(machine.guest_fpsr() == initial_fpsr);
  machine.require_preserved();
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  if (uml::cpu::arm64::jit::sme_support == uml::cpu::arm64::SmeSupport::none ||
      !jit_available() ||
      !uml::cpu::arm64::implements_sme(uml::cpu::host_capabilities())) {
    std::printf("skipped: SM=1 code needs a host profile with SME\n");
    return 0;
  }
  general_registers_reach_their_fields();
  flags_follow_the_guest();
  za_forms_take_their_slice_indexes();
  fpcr_controls_streaming_floating_point();
  fp_flags_survive_kernel_entries();
  movprfx_is_a_move();
  return 0;
}
