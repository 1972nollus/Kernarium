// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The JIT's C++ side of the SME control core: guest writes of SVCR and the
 * slow path of the ZA and ZT0 forms. Generated code reaches both as
 * terminal helpers; an SM=1 translation's leave sequence has spilled the
 * guest's vector state to the register file first, so both work on
 * CpuState alone.
 */
#include "jit_semantic_memory.hpp"
#include "jit_semantic_reservation.hpp"
#include "jit_sme_support.hpp"

#include "cpu/arm64/fp_environment.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

namespace uml::cpu::arm64 {

namespace {

using ::uml::memory::Access;

void materialize(CpuState &cpu) noexcept {
  arm64::restore_native_fp_environment(cpu);
  cpu.access_fp_state();
}

[[nodiscard]] std::uint64_t retire(CpuState &cpu) noexcept {
  cpu.pc += sizeof(std::uint32_t);
  return pack_jit_result(Event::continued, 1);
}

[[nodiscard]] std::uint64_t report_fault(Tlb &tlb, std::uint64_t address,
                                         bool load,
                                         bool alignment) noexcept {
  tlb.fault_address = address;
  tlb.fault_access = load ? Access::read : Access::write;
  tlb.fault_alignment = alignment ? 1 : 0;
  return pack_jit_result(Event::memory_fault, 0);
}

/*
 * Moves `size` bytes (at most one SVL, below a page) between guest memory
 * at `address` and `file`. Every page is translated before any byte moves,
 * so a store is never half applied, and a fault reports the first byte of
 * the first page that fails, as the M4 does (the S3b native records). A
 * store revalidates write permission at this boundary, invalidates the
 * software reservations it overlaps and clears the local monitor, as the
 * native fast path does.
 */
[[nodiscard]] std::uint64_t transfer(CpuState &cpu, Tlb &tlb,
                                     std::uint64_t address, std::byte *file,
                                     std::size_t size, bool load) noexcept {
  std::array<std::byte *, 2> host{};
  std::array<std::size_t, 2> lengths{};
  std::size_t pages = 0;
  for (std::size_t completed = 0; completed < size;) {
    const auto current = address + completed;
    if (pages == host.size())
      return pack_jit_result(Event::fatal, 0);
    if (!load)
      tlb.discard(current);
    auto *pointer = tlb.translate(current, load ? Access::read : Access::write);
    if (pointer == nullptr)
      return report_fault(tlb, current, load, false);
    const auto length = std::min(
        size - completed,
        static_cast<std::size_t>(page_size - (current & (page_size - 1U))));
    host[pages] = pointer;
    lengths[pages] = length;
    ++pages;
    completed += length;
  }
  std::size_t completed = 0;
  std::array<JitStoreRange, 2> stored{};
  for (std::size_t page = 0; page < pages; ++page) {
    if (load) {
      std::memcpy(file + completed, host[page], lengths[page]);
    } else {
      std::memcpy(host[page], file + completed, lengths[page]);
      stored[page] = {reinterpret_cast<std::uintptr_t>(host[page]),
                      lengths[page]};
    }
    completed += lengths[page];
  }
  if (!load) {
    jit::semantic::invalidate_store_ranges(
        std::span<const JitStoreRange>{stored}.first(pages));
    cpu.exclusive_size = 0;
  }
  return retire(cpu);
}

} // namespace

extern "C" std::uint64_t
uml_arm64_jit_write_svcr(CpuState *cpu, Tlb *tlb,
                         std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr || cpu->sme_registers == nullptr)
    return pack_jit_result(Event::fatal, 0);
  std::uint8_t mask = svcr_mask;
  std::uint8_t value = 0;
  if ((instruction & 0xffff'ffe0U) == 0xd51b'4240U) {
    /* MSR SVCR, Xt: SM from bit 0, ZA from bit 1. */
    value = static_cast<std::uint8_t>(
        cpu->read_register(instruction & 31U, false) & svcr_mask);
  } else if ((instruction & 0xffff'f0ffU) == 0xd503'407fU) {
    /* MSR SVCRSM, SVCRZA or SVCRSMZA, #imm: CRm<2:1> selects, CRm<0> is
     * the value. Translation routes only those three selections here. */
    const unsigned field = (instruction >> 8) & 15U;
    mask = static_cast<std::uint8_t>(((field >> 1) & 1U ? svcr_sm : 0U) |
                                     ((field >> 2) & 1U ? svcr_za : 0U));
    if (mask == 0U || field > 7U)
      return pack_jit_result(Event::fatal, 0);
    value = (field & 1U) != 0U ? mask : std::uint8_t{0};
  } else {
    return pack_jit_result(Event::fatal, 0);
  }
  /* write_svcr() needs CpuState to hold the newest guest FPSR. */
  materialize(*cpu);
  if (!cpu->write_svcr(mask, value))
    return pack_jit_result(Event::sme_access, 0);
  return retire(*cpu);
}

extern "C" std::uint64_t
uml_arm64_jit_execute_sme(CpuState *cpu, Tlb *tlb,
                          std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr || cpu->sme_registers == nullptr ||
      !valid_streaming_vector_length(cpu->sme_svl))
    return pack_jit_result(Event::fatal, 0);
  const auto *row = detail::find_sme_encoding(instruction);
  if (row == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize(*cpu);
  const std::size_t svl = cpu->sme_svl;
  auto *const za = cpu->sme_registers + sme_za_offset(svl, 0);
  auto *const zt0 = cpu->sme_registers + sme_zt0_offset(svl);
  const auto form = row->form;
  if (form == jit::sme_forms::zero_za) {
    const unsigned tiles = instruction & 0xffU;
    for (std::size_t vector = 0; vector < svl; ++vector)
      if (((tiles >> (vector % 8U)) & 1U) != 0U)
        std::memset(za + vector * svl, 0, svl);
    return retire(*cpu);
  }
  if (form == jit::sme_forms::zero_zt0) {
    std::memset(zt0, 0, sme_zt0_bytes);
    return retire(*cpu);
  }
  const bool array_vector =
      form == jit::sme_forms::ldr_za || form == jit::sme_forms::str_za;
  if (!array_vector && form != jit::sme_forms::ldr_zt0 &&
      form != jit::sme_forms::str_zt0)
    return pack_jit_result(Event::fatal, 0);
  const bool load =
      form == jit::sme_forms::ldr_za || form == jit::sme_forms::ldr_zt0;
  const unsigned base = (instruction >> 5) & 31U;
  const auto tagged = cpu->read_register(base, true);
  /* CheckSPAlignment() comes before any access. */
  if (base == 31U && (tagged & 15U) != 0U)
    return report_fault(*tlb, tagged, load, true);
  auto address = cpu->untag_data_address(tagged, base == 31U);
  if (!array_vector)
    return transfer(*cpu, *tlb, address, zt0, sme_zt0_bytes, load);
  /* LDR and STR ZA[Wv, #off4], [Xn|SP, #off4, MUL VL]: one horizontal ZA
   * vector, (Wv + off4) MOD SVL, at Xn + off4 * SVL. */
  const unsigned offset = instruction & 15U;
  const auto index = static_cast<std::uint32_t>(
      cpu->registers[12U + ((instruction >> 13) & 3U)]);
  const auto vector = (static_cast<std::size_t>(index) + offset) % svl;
  address += offset * svl;
  return transfer(*cpu, *tlb, address, za + vector * svl, svl, load);
}

} // namespace uml::cpu::arm64
