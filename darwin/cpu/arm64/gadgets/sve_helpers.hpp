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

/* Runs `word`, implemented as `row`, through the semantic library on `cpu`,
 * reaching guest memory through `tlb`; a fault is recorded in the TLB, as
 * the memory-fault exit expects. CpuState must hold the guest's NZCV and FP
 * state. */
[[nodiscard]] detail::SveResult execute_sve_word(CpuState &cpu,
	memory::Tlb &tlb, std::uint32_t word,
	const detail::SmeEncoding &row) noexcept;

} // namespace uml::cpu::arm64::gadgets
