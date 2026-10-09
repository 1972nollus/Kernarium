// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/detail/sme_encodings.hpp"
#include "cpu/arm64/detail/sve_semantics.hpp"
#include "cpu/host_capabilities.hpp"
#include "memory/tlb.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64::gadgets {

/* How the engine treats a word of the SVE/SME space in a program translated
 * for SVCR.SM = 0 on a virtual CPU with the SVE profile. */
enum class SveWord : std::uint8_t {
	/* An SME row, or a word of no row: the SME decoder's. */
	other,
	/* The SVE path, but the virtual CPU does not implement the row, it
	 * needs PSTATE.ZA, which is clear, or the word has a reserved bitmask
	 * immediate: UNDEFINED. */
	undefined,
	/* The SVE path, implemented: `row` is the word's row. */
	implemented,
};

struct SveWordDecode final {
	SveWord kind{SveWord::other};
	const detail::SmeEncoding *row{};
};

/* The SVE path is a row whose non-streaming requirement holds FEAT_SVE;
 * the virtual CPU implements it when it has every part of that
 * requirement (detail::implemented_sve_features). */
[[nodiscard]] SveWordDecode decode_sve_word(std::uint32_t word,
	const HostCapabilities &host, std::uint8_t svcr) noexcept;

/* The library-exit report: how many words of each table row the engines of
 * this process have run through the semantic library, by the sve_library
 * gadget or the executor's trap-enabled path, while counting is enabled
 * (it is off by default, since every vCPU would share the counters) and
 * since the last reset. A form a generated gadget runs on its fast path is
 * not counted, and neither is the corpus's library lane. Index rows as
 * detail::sme_encodings. Counts are relaxed; read them when the engines
 * are quiet. */
void count_sve_library_exits(bool enable) noexcept;
[[nodiscard]] std::uint64_t sve_library_exits(std::size_t row) noexcept;
[[nodiscard]] std::uint64_t sve_library_exit_total() noexcept;
void reset_sve_library_exits() noexcept;

/* The same counting also tallies, per row, the SVE words the builder
 * translated and how many of those it sent to the library for want of a
 * generated gadget (their executions are the exits). The builder calls
 * note_sve_translation; the report reads the counts. */
void note_sve_translation(std::size_t row, bool library) noexcept;
[[nodiscard]] std::uint64_t sve_translated(std::size_t row) noexcept;
[[nodiscard]] std::uint64_t sve_translated_to_library(
	std::size_t row) noexcept;

/* Writes the report to `path`: a '#' header, then one tab-separated line
 * per row with a count, most exits first:
 *   SVE-nnnn  row  mask  value  translated  translated_to_library  exits
 * The form number is the architecture oracle's, the mask and value are the
 * row's encoding in hex. tools/arm64-tests/sve_library_report.py names the
 * forms. Setting DARWIN_UML_SVE_LIBRARY_EXITS to a path in a process that
 * links the gadget engine enables counting from its start and writes the
 * report there at exit, and again each time a gadget executor retires,
 * because the emulator leaves through _Exit, which runs no atexit handler:
 * the last write is the complete one. Returns false if the file cannot be
 * written. */
bool write_sve_library_report(const char *path) noexcept;

/* Rewrites the report the environment asked for, if it did, from the counts
 * so far. The executor calls it as it retires. */
void write_sve_library_report_from_environment() noexcept;

/* Runs `word`, implemented as `row`, through the semantic library on `cpu`,
 * reaching guest memory through `tlb`; a fault is recorded in the TLB, as
 * the memory-fault exit expects. CpuState must hold the guest's NZCV and FP
 * state. */
[[nodiscard]] detail::SveResult execute_sve_word(CpuState &cpu,
	memory::Tlb &tlb, std::uint32_t word,
	const detail::SmeEncoding &row) noexcept;

} // namespace uml::cpu::arm64::gadgets
