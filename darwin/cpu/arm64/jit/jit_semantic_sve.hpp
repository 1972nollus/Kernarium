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

/*
 * The library-exit report: how many words of each table row the JIT's
 * engines in this process have run through the semantic library, by the
 * terminal helper or the executor's trapped-exception mode, while counting
 * is enabled and since the last reset. A word the translation lowers
 * inline, and a word it ends in the illegal-instruction exit, is not
 * counted. Counting is off by default, so that no vCPU shares a counter
 * unless a report asks. Index rows as detail::sme_encodings; a row's last
 * word is kept so that a report can name what ran. Counts are relaxed:
 * read them when the engines are quiet.
 *
 * The gadget engine writes the same report: a translation of a row counts
 * once (`note_sve_translation`), and once more as sent to the library when
 * its translation ends the block there.
 */
void count_sve_library_exits(bool enable) noexcept;
[[nodiscard]] std::uint64_t sve_library_exits(std::size_t row) noexcept;
[[nodiscard]] std::uint32_t sve_library_exit_word(std::size_t row) noexcept;
[[nodiscard]] std::uint64_t sve_library_exit_total() noexcept;
void reset_sve_library_exits() noexcept;

void note_sve_translation(std::size_t row, bool library) noexcept;
[[nodiscard]] std::uint64_t sve_translated(std::size_t row) noexcept;
[[nodiscard]] std::uint64_t sve_translated_to_library(std::size_t row) noexcept;

/*
 * Writes the report to `path`: a '#' header, then one tab-separated line per
 * row with a count, most exits first:
 *   SVE-nnnn  row  mask  value  translated  translated_to_library  exits
 * The form number is the architecture oracle's, and the mask and value are
 * the row's encoding in hex. tools/arm64-tests/sve-library-report.py names
 * the forms. Setting DARWIN_UML_SVE_LIBRARY_EXITS to a path enables counting
 * from the process's start, writes the report there as the process ends and
 * writes it again whenever an executor retires, because the emulator leaves
 * through _Exit, which runs no exit handler: the last write is the complete
 * one. Returns false if the file cannot be written.
 */
bool write_sve_library_report(const char *path) noexcept;

/* Rewrites the report the environment asked for, if it did, from the counts
 * so far. The executor calls it as it retires. */
void write_sve_library_report_from_environment() noexcept;

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
