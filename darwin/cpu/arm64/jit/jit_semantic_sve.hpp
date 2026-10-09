// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_runtime_contract.hpp"

#include "cpu/arm64/detail/sve_semantics.hpp"
#include "cpu/host_capabilities.hpp"
#include "memory/tlb.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace uml::cpu::arm64::jit::semantic {

namespace sve_detail = ::uml::cpu::arm64::detail;

/*
 * Runs `word`, implemented as `row`, through the engine-neutral semantic
 * library (detail::execute_sve) on `cpu`, reaching guest memory through
 * `tlb`. A fault is recorded in the TLB, as the memory-fault exit expects,
 * and every store invalidates the software reservations it overlaps. The
 * caller has left the native FP environment (restore_native_fp_environment)
 * so CpuState holds the guest's NZCV and FP state; the library makes the
 * lazily loaded FP state accessible itself.
 */
[[nodiscard]] sve_detail::SveResult execute_sve_word(
    CpuState &cpu, ::uml::memory::Tlb &tlb, std::uint32_t word,
    const sve_detail::SmeEncoding &row) noexcept;

/* The outcome of one SVE word run on its own. */
struct SveWordRun final {
  Event event;
  std::size_t executed;
  /* For a floating-point trap: the ESR syndrome. */
  std::uint32_t syndrome;
};

/*
 * The executor's trapped-exception and single-step mode: runs `word`
 * outside streaming mode (`svcr` is the guest's) through the library when
 * the virtual CPU implements it, and reports the event with PC advanced on
 * retirement. The library decides a floating-point form's trap and the
 * flags it commits, so nothing commits on a trap and exactly one cause is
 * reported (detail::decide_floating_exception). A word of no implemented
 * row, or one the table allocates to SME, gives nothing: the translator's
 * decision stands. A word UNDEFINED by its encoding is the
 * illegal-instruction exit with nothing changed.
 */
[[nodiscard]] std::optional<SveWordRun> run_sve_word(
    CpuState &cpu, ::uml::memory::Tlb &tlb, std::uint32_t word,
    const ::uml::cpu::HostCapabilities &host) noexcept;

} // namespace uml::cpu::arm64::jit::semantic
