// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/arm64/sve_profile.hpp"
#include "cpu/host_capabilities.hpp"
#include "memory/guest_address_space.hpp"

#include <cstddef>
#include <cstdint>

/*
 * The engine-neutral semantics of the SVE profile outside streaming mode, at
 * the virtual CPU's fixed vector length of 16 bytes (sve_profile.hpp). One
 * C++ implementation of every SVE-index encoding the profile executes at
 * PSTATE.SM = 0, driven by the generated encoding table: an engine calls it
 * for the forms it does not lower inline, for its memory slow paths and for
 * its floating-point forms in the trapped-exception mode, and the instruction
 * corpus checks it directly in a lane of its own.
 *
 * It works on CpuState alone: Z_n is `vectors[n]`, P and FFR are the mask
 * form in `sve_p` and `sve_ffr`, and memory is reached only through the
 * caller's SveMemory. It includes no engine header, so either engine can
 * call it without depending on the other.
 */

namespace uml::cpu::arm64::detail {

/*
 * The table rows the virtual CPU implements on `host` outside streaming
 * mode, as their non-streaming requirement bits: none without the SVE
 * profile; otherwise its SVE tokens, BF16 and I8MM where the host has them
 * (their SVE forms need sve as well), and the SME profile's parts, which
 * admit the SCLAMP class at PSTATE.SM = 0 when the CPU has SME (and SME2
 * for 25 of them) besides SVE.
 */
[[nodiscard]] inline SmeFeatureMask implemented_sve_features(
	const HostCapabilities &host) noexcept
{
	if (!implements_sve(host))
		return {};
	ArmFeatureSet features;
	for (const ArmFeature feature : sve_features)
		if (host.has(feature))
			features.set(static_cast<std::size_t>(feature));
	for (const ArmFeature feature : {ArmFeature::bf16, ArmFeature::i8mm})
		if (host.has(feature))
			features.set(static_cast<std::size_t>(feature));
	return static_cast<SmeFeatureMask>(
		sme_feature_mask(features) | implemented_sme_features(host));
}

/*
 * The row of an SVE-index word that a CPU with these non-streaming features
 * executes at PSTATE.SM = 0, or nullptr: the word is UNDEFINED there, or it
 * is an SME-index word, which the SME control cores own.
 */
[[nodiscard]] constexpr const SmeEncoding *decode_sve_non_streaming(
	std::uint32_t word, SmeFeatureMask features) noexcept
{
	const SmeEncoding *row = find_sme_encoding(word);
	if (row == nullptr || (row->form & sme_form_sme_section) != 0U ||
		row->non_streaming_requirement == 0U ||
		(row->non_streaming_requirement & features) !=
			row->non_streaming_requirement)
		return nullptr;
	return row;
}

/*
 * Guest memory as the caller's engine reaches it. translate() returns the
 * host address of the guest byte at `address` for `access`, valid through
 * the end of its guest page, or nullptr when the page does not translate
 * that way; it records no fault, since a first-fault or non-fault load
 * only asks. After a store has moved bytes the library reports each host
 * range it wrote to stored(), so an engine can invalidate the reservations
 * they overlap. What a faulting access leaves in memory and in its
 * destination is what the hardware leaves (the corpus's native records).
 */
class SveMemory {
public:
	[[nodiscard]] virtual std::byte *translate(std::uint64_t address,
		::uml::memory::MemoryAccess access) noexcept = 0;
	virtual void stored(std::byte *host, std::size_t size) noexcept = 0;

protected:
	SveMemory() = default;
	~SveMemory() = default;
	SveMemory(const SveMemory &) = default;
	SveMemory &operator=(const SveMemory &) = default;
};

enum class SveEvent : std::uint8_t {
	/* The instruction completed; the caller advances PC. */
	retired,
	/* A data abort or alignment fault, at fault_address for access. */
	memory_fault,
	/* An enabled floating-point exception, with its ESR syndrome. */
	floating_point_trap,
	/* The pseudocode makes this value of a field the table leaves free
	 * UNDEFINED (a reserved bitmask immediate): the illegal-instruction
	 * exit, which changes nothing. */
	undefined,
	/* The word is outside the library's contract: an internal error. */
	fatal,
};

struct SveResult final {
	SveEvent event{SveEvent::retired};
	/* For memory_fault: an alignment fault (an SP base that is not 16-byte
	 * aligned) rather than a translation fault. */
	bool alignment{};
	::uml::memory::MemoryAccess access{::uml::memory::MemoryAccess::read};
	std::uint64_t fault_address{};
	std::uint32_t syndrome{};
};

/*
 * Whether execute_sve() implements the row, which holds for every row
 * decode_sve_non_streaming() can return. The instruction corpus's library
 * lane counts as skipped the cases of any row it does not hold for, which
 * only a regenerated table with a new form can produce.
 */
[[nodiscard]] bool sve_semantics_covers(const SmeEncoding &row) noexcept;

/*
 * Executes `word`, which decode_sve_non_streaming() decoded to `row` for
 * the virtual CPU's features, at PSTATE.SM = 0. The caller holds no newer
 * guest FP state in a native FP environment than CpuState does; the library
 * makes the lazily loaded FP state accessible itself, marks the task's SVE
 * state live unless the word is undefined, and never changes PC. A
 * floating-point form raises the cumulative FPSR flags of its active
 * elements only, and while an FPCR trap enable is set it traps when one of
 * them raises an enabled exception, as AdvSIMD's transactional path does.
 * A fault or a floating-point trap leaves the registers as they were, but
 * for the cumulative FPSR flags a trap commits and the live mark, and a
 * faulting store leaves memory as the hardware does (SveMemory). A store
 * that writes, or completes, clears the local exclusive monitor.
 * Any other word, or a row the library does not cover, is a fatal internal
 * error that changes nothing.
 */
[[nodiscard]] SveResult execute_sve(CpuState &cpu, std::uint32_t word,
	const SmeEncoding &row, SveMemory &memory) noexcept;

} // namespace uml::cpu::arm64::detail
