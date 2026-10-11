// SPDX-License-Identifier: GPL-2.0-or-later
/* The JIT's VFP and Advanced SIMD behaviour that the instruction corpus does
 * not see. The floating-point environment: a VFP instruction runs under the
 * guest's FPCR and an Advanced SIMD one under the Standard FPSCR value, across
 * a conditional arm, a direct branch (a chained block) and an exit. The
 * cumulative saturation flag. The vector memory forms: that they stay out of
 * the semantic library on an ordinary access, and take it, with its data and
 * its faults, on a page crossing, a misalignment and an unmapped page. The
 * words were assembled with LLVM MC at authoring time. */
#include "arm64_jit_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/jit/a32/a32_helpers.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace uml::emulator::tests;
using namespace uml::emulator::tests::a32;
using ::uml::cpu::ExitReason;

namespace {

namespace jit_a32 = ::uml::cpu::arm64::jit::a32;

constexpr std::uint32_t arm_svc = 0xef00'0000U;
constexpr std::uint32_t arm_nop = 0xe320'f000U;
constexpr std::uint32_t fpscr_qc = 1U << 27;

/* The lane `lane` (32 bits) of Q register `q`. */
std::uint32_t lane32(const arm64::CpuState &cpu, unsigned q, unsigned lane) {
  std::uint32_t value;
  std::memcpy(&value, cpu.vectors[q].bytes.data() + 4U * lane, 4);
  return value;
}
void set_lane32(arm64::CpuState &cpu, unsigned q, unsigned lane,
                std::uint32_t value) {
  std::memcpy(cpu.vectors[q].bytes.data() + 4U * lane, &value, 4);
}
std::uint64_t half64(const arm64::CpuState &cpu, unsigned q, unsigned half) {
  std::uint64_t value;
  std::memcpy(&value, cpu.vectors[q].bytes.data() + 8U * half, 8);
  return value;
}
void set_half64(arm64::CpuState &cpu, unsigned q, unsigned half,
                std::uint64_t value) {
  std::memcpy(cpu.vectors[q].bytes.data() + 8U * half, &value, 8);
}

/* Whether the program ran without a trip to the semantic library. */
struct ExitCount final {
  ExitCount() {
    jit_a32::reset_helper_exits();
    jit_a32::count_helper_exits(true);
  }
  ~ExitCount() { jit_a32::count_helper_exits(false); }
  [[nodiscard]] static std::size_t total() { return jit_a32::helper_exit_total(); }
};

// ---------------------------------------------------------------------------
// The floating-point environment.

constexpr std::uint32_t one = 0x3f80'0000U;
/* 1.5 * 2^-24: 1 + this is 3/4 of a unit in the last place above 1, which
 * rounds up to nearest and down toward zero. */
constexpr std::uint32_t three_quarters_ulp = 0x33c0'0000U;
constexpr std::uint32_t one_ulp_up = 0x3f80'0001U;
constexpr std::uint32_t sentinel = 0xdead'beefU;
constexpr std::uint32_t round_toward_zero = 0x00c0'0000U;

void load_fp_program(Fixture &fixture) {
  fixture.code32(0, {
      0xeee1'1a10U, // vmsr fpscr, r1
      0xee30'0a81U, // vadd.f32 s0, s1, s2     (VFP: the guest's FPCR)
      0xf204'3d05U, // vadd.f32 d3, d4, d5     (Neon: Standard FPSCR)
      0x1e30'8a81U, // vaddne.f32 s16, s1, s2  (a conditional arm)
      0xf204'7d05U, // vadd.f32 d7, d4, d5
      0x0e30'aa81U, // vaddeq.f32 s20, s1, s2
      0xea00'0000U, // b +1 (a direct branch: the next block is chained)
      arm_nop,
      0xee30'ca81U, // vadd.f32 s24, s1, s2    (the first of a block)
      arm_svc});
}

arm64::CpuState fp_task(std::uint32_t rounding, bool zero) {
  auto cpu = Fixture::task(false);
  cpu.registers[1] = rounding;
  cpu.nzcv = zero ? flag_z : 0U;
  set_lane32(cpu, 0, 0, 0x1111'1111U);
  set_lane32(cpu, 0, 1, one);
  set_lane32(cpu, 0, 2, three_quarters_ulp);
  set_lane32(cpu, 0, 3, 0x2222'2222U);
  set_half64(cpu, 1, 0, 0x4444'4444'4444'4444U);
  set_half64(cpu, 1, 1, 0x4545'4545'4545'4545U);
  set_half64(cpu, 2, 0, std::uint64_t{one} << 32 | one);               // d4
  set_half64(cpu, 2, 1, std::uint64_t{three_quarters_ulp} << 32 |
                            three_quarters_ulp);                        // d5
  for (const unsigned q : {3U, 4U, 5U, 6U})
    for (unsigned lane = 0; lane < 4; ++lane)
      set_lane32(cpu, q, lane, sentinel);
  return cpu;
}

void floating_point_environment() {
  Fixture fixture{0x1a32'0d01};
  load_fp_program(fixture);
  for (const bool toward_zero : {true, false}) {
    const std::uint32_t vfp_sum = toward_zero ? one : one_ulp_up;
    for (const bool zero : {false, true}) {
      // The second round of each pair runs the chained translations.
      for (unsigned round = 0; round < 2; ++round) {
        auto cpu = fp_task(toward_zero ? round_toward_zero : 0U, zero);
        const auto result = fixture.run(cpu);
        require(result.reason == ExitReason::system_call);
        require(lane32(cpu, 0, 0) == vfp_sum);
        // The Neon sums are nearest whatever the guest's rounding mode.
        require(half64(cpu, 1, 1) ==
                (std::uint64_t{one_ulp_up} << 32 | one_ulp_up));
        require(half64(cpu, 1, 0) == 0x4444'4444'4444'4444U);
        require(half64(cpu, 3, 1) ==
                (std::uint64_t{one_ulp_up} << 32 | one_ulp_up));
        require(half64(cpu, 3, 0) == (std::uint64_t{sentinel} << 32 | sentinel));
        require(lane32(cpu, 4, 0) == (zero ? sentinel : vfp_sum));
        require(lane32(cpu, 5, 0) == (zero ? vfp_sum : sentinel));
        require(lane32(cpu, 6, 0) == vfp_sum);
        // The guest's FPCR, not the Standard value, is what an exit leaves.
        require(cpu.fpcr == (toward_zero ? round_toward_zero : 0U));
        require((cpu.fpsr & fpscr_qc) == 0U);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// The cumulative saturation flag.

void saturation_flag() {
  Fixture fixture{0x1a32'0d02};
  fixture.code32(0, {
      0xf287'0e1fU, // vmov.i8 d0, #0x7f
      0xf280'1e11U, // vmov.i8 d1, #1
      0xf201'2011U, // vqadd.s8 d2, d1, d1
      0xeef1'0a10U, // vmrs r0, fpscr
      0xf200'3011U, // vqadd.s8 d3, d0, d1
      0xeef1'1a10U, // vmrs r1, fpscr
      arm_svc});
  auto cpu = Fixture::task(false);
  auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[0] == 0U);
  require(cpu.registers[1] == fpscr_qc);
  require(half64(cpu, 1, 1) == 0x7f7f'7f7f'7f7f'7f7fU); // d3
  require(half64(cpu, 1, 0) == 0x0202'0202'0202'0202U); // d2
  require((cpu.fpsr & fpscr_qc) != 0U);

  // The flag is cumulative: a run that starts with it set keeps it, and
  // reads it before it saturates.
  cpu = Fixture::task(false);
  cpu.fpsr = fpscr_qc;
  result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(cpu.registers[0] == fpscr_qc);
  require((cpu.fpsr & fpscr_qc) != 0U);
}

// ---------------------------------------------------------------------------
// Vector memory.

constexpr std::size_t source_offset = 0x0;
constexpr std::size_t copy_offset = 0x400;
constexpr std::size_t structure_offset = 0x100;
constexpr std::size_t result_offset = 0x500;

std::uint8_t pattern(std::size_t index) {
  return static_cast<std::uint8_t>(index * 7U + 3U);
}

void fill_pattern(Fixture &fixture, std::size_t bytes) {
  std::vector<std::byte> data(bytes);
  for (std::size_t index = 0; index < bytes; ++index)
    data[index] = static_cast<std::byte>(pattern(index));
  fixture.data_bytes(0, data);
}

void load_memory_program(Fixture &fixture) {
  fixture.code32(0, {
      0xecb0'0b08U, // vldmia r0!, {d0-d3}
      0xeca1'0b08U, // vstmia r1!, {d0-d3}
      0xf422'400dU, // vld4.8 {d4-d7}, [r2]!
      0xf405'420fU, // vst1.8 {d4-d7}, [r5]
      0xed10'8b02U, // vldr d8, [r0, #-8]
      0xed01'aa01U, // vstr s20, [r1, #-4]
      arm_svc});
}

arm64::CpuState memory_task(std::size_t structure_source) {
  auto cpu = Fixture::task(false);
  cpu.registers[0] = data_base + source_offset;
  cpu.registers[1] = data_base + copy_offset;
  cpu.registers[2] = data_base + static_cast<std::uint32_t>(structure_source);
  cpu.registers[5] = data_base + result_offset;
  for (unsigned lane = 0; lane < 4; ++lane)
    set_lane32(cpu, 5, lane, 0xa5a5'a5a5U);
  return cpu;
}

/* The program's effect on memory and registers, from the pattern. */
void check_memory_program(Fixture &fixture, const arm64::CpuState &cpu,
                          std::size_t structure_source) {
  const auto copied = fixture.read_data(copy_offset, 32);
  for (std::size_t index = 0; index < 28; ++index)
    require(static_cast<std::uint8_t>(copied[index]) == pattern(index));
  // The last word is the vstr of s20 over the end of the block store.
  require(fixture.read32(copy_offset + 28) == 0xa5a5'a5a5U);
  require(cpu.registers[0] == data_base + 32U);
  require(cpu.registers[1] == data_base + copy_offset + 32U);
  require(cpu.registers[2] == data_base + structure_source + 32U);
  // vld4.8 de-interleaves four byte structures into d4-d7; vst1.8 writes them
  // out in register order.
  const auto stored = fixture.read_data(result_offset, 32);
  for (std::size_t reg = 0; reg < 4; ++reg)
    for (std::size_t element = 0; element < 8; ++element)
      require(static_cast<std::uint8_t>(stored[8 * reg + element]) ==
              pattern(structure_source + 4 * element + reg));
  // vldr d8, [r0, #-8] loads the block's last doubleword.
  std::uint64_t last = 0;
  for (std::size_t index = 0; index < 8; ++index)
    last |= std::uint64_t{pattern(24 + index)} << (8 * index);
  require(half64(cpu, 4, 0) == last);
}

void vector_memory_stays_inline() {
  Fixture fixture{0x1a32'0d03};
  load_memory_program(fixture);
  fill_pattern(fixture, 2 * guest_page);
  auto cpu = memory_task(structure_offset);
  ExitCount exits;
  const auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(result.retired_instructions == 7);
  require(ExitCount::total() == 0);
  check_memory_program(fixture, cpu, structure_offset);
}

void vector_memory_across_a_page() {
  Fixture fixture{0x1a32'0d04};
  load_memory_program(fixture);
  fill_pattern(fixture, 2 * guest_page);
  // The 32-byte structure load starts 16 bytes before the end of the page.
  const std::size_t crossing = guest_page - 16;
  auto cpu = memory_task(crossing);
  ExitCount exits;
  const auto result = fixture.run(cpu);
  require(result.reason == ExitReason::system_call);
  require(result.retired_instructions == 7);
  require(ExitCount::total() != 0); // the access is the library's
  check_memory_program(fixture, cpu, crossing);
}

/* One instruction at `address`, run from fresh registers; the faulting access
 * must leave the registers it would have written alone. */
void vector_memory_faults() {
  const auto sentinel_registers = [](arm64::CpuState &cpu) {
    set_half64(cpu, 0, 0, 0x0123'4567'89ab'cdefU);
    set_half64(cpu, 0, 1, 0xfedc'ba98'7654'3210U);
  };
  const std::uint32_t unmapped = data_base + 2U * static_cast<std::uint32_t>(guest_page);

  {
    // A load that starts on the last mapped bytes and ends past them faults
    // on the first byte of the page that is not there.
    Fixture fixture{0x1a32'0d05};
    fixture.code32(0, {0xf426'0a0fU, arm_svc}); // vld1.8 {d0, d1}, [r6]
    auto cpu = Fixture::task(false);
    cpu.registers[6] = unmapped - 8U;
    sentinel_registers(cpu);
    const auto result = fixture.run(cpu);
    require(result.reason == ExitReason::memory_fault);
    require(result.retired_instructions == 0);
    require(result.fault.address == unmapped);
    require(result.fault.access == ::uml::memory::MemoryAccess::read);
    require(!result.fault.alignment);
    require(half64(cpu, 0, 0) == 0x0123'4567'89ab'cdefU);
    require(half64(cpu, 0, 1) == 0xfedc'ba98'7654'3210U);
  }
  {
    // A misaligned access that names its alignment.
    Fixture fixture{0x1a32'0d06};
    fixture.code32(0, {0xf426'079fU, arm_svc}); // vld1.32 {d0}, [r6:64]
    auto cpu = Fixture::task(false);
    cpu.registers[6] = data_base + 4U;
    sentinel_registers(cpu);
    const auto result = fixture.run(cpu);
    require(result.reason == ExitReason::memory_fault);
    require(result.retired_instructions == 0);
    require(result.fault.address == data_base + 4U);
    require(result.fault.alignment);
    require(half64(cpu, 0, 0) == 0x0123'4567'89ab'cdefU);
  }
  {
    // A store that would cross into the unmapped page writes no byte.
    Fixture fixture{0x1a32'0d07};
    fixture.code32(0, {0xf406'084fU, arm_svc}); // vst2.16 {d0, d1}, [r6]
    for (std::size_t index = 0; index < 8; ++index)
      fixture.data_bytes(2 * guest_page - 8 + index,
                         std::array<std::byte, 1>{std::byte{0x5a}});
    auto cpu = Fixture::task(false);
    cpu.registers[6] = unmapped - 8U;
    sentinel_registers(cpu);
    const auto result = fixture.run(cpu);
    require(result.reason == ExitReason::memory_fault);
    require(result.retired_instructions == 0);
    require(result.fault.address == unmapped);
    require(result.fault.access == ::uml::memory::MemoryAccess::write);
    for (const auto byte : fixture.read_data(2 * guest_page - 8, 8))
      require(byte == std::byte{0x5a});
  }
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(jit_available() == 1);
  floating_point_environment();
  saturation_flag();
  vector_memory_stays_inline();
  vector_memory_across_a_page();
  vector_memory_faults();
  std::puts("arm64-jit-a32-vector: ok");
  return 0;
}
