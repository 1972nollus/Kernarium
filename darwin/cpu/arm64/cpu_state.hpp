// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace uml::cpu::arm64 {

/* The FPCR bits an MSR FPCR, a signal-frame or ptrace restore, or any other
 * context load can set; the rest read as zero and ignore writes. From the
 * AArch64 FPCR description (DDI 0601, 2026-06), for this virtual CPU:
 * - AHP, DN, FZ and RMode are unconditional fields;
 * - FZ16 exists with FEAT_FP16, which every supported host provides;
 * - FIZ, AH and NEP exist only with FEAT_AFP, which the virtual CPU does not
 *   implement, even on a host that has it (fpcr_afp_controls);
 * - EBF exists only with FEAT_EBF16, which ID_AA64ISAR1_EL1.BF16 = 1 omits;
 * - the trap enables are RAZ/WI only without trapped exception handling,
 *   which both engines implement;
 * - Len and Stride serve only to save and restore the AArch32 FPSCR, and
 *   whether they are programmable or RAZ/WI is IMPLEMENTATION DEFINED.
 *   Without AArch32 they are RAZ/WI.
 * This is the one definition of the mask; the JIT, the context restore
 * path and the tests read it, and the gadget assembly's
 * UML_ARM64_FPCR_WRITABLE is checked against it. */
inline constexpr std::uint32_t fpcr_writable_mask = 0x07c8'9f00U;

/* FPCR.NEP, AH and FIZ: RES0 because the virtual CPU omits FEAT_AFP. */
inline constexpr std::uint32_t fpcr_afp_controls = 0x0000'0007U;
static_assert((fpcr_writable_mask & fpcr_afp_controls) == 0U);

struct alignas(16) VectorRegister final {
	std::array<std::byte, 16> bytes{};
};

struct CpuState final {
	std::array<std::uint64_t, 31> registers{};
	std::uint64_t sp{};
	std::uint64_t pc{};
	std::uint32_t nzcv{};
	std::array<VectorRegister, 32> vectors{};
	std::uint32_t fpcr{};
	std::uint32_t fpsr{};
	std::uint64_t tls{};
	/* Effective TBI0, imported from the task: EL0 data addresses ignore
	 * their top byte. SP-based addresses are never untagged. */
	bool tbi_enabled{};
	/* EL0-visible PSTATE fields controlled by MSR (immediate).  DAIF access is
	 * explicitly gated because SCTLR_EL1.UMA determines whether userspace may
	 * use DAIFSet/DAIFClr; the other fields are effective feature state. */
	bool pstate_ssbs{};
	bool pstate_dit{};
	bool pstate_sm{};
	bool pstate_za{};
	bool pstate_daif_access{};
	std::uint8_t pstate_daif{};
	std::uint64_t exclusive_address{~std::uint64_t{0}};
	std::uintptr_t exclusive_key{};
	std::uint64_t exclusive_value{};
	std::uint64_t exclusive_value2{};
	std::uint64_t exclusive_version{};
	std::uint8_t exclusive_size{};
	const std::byte *deferred_vectors{};
	std::uint32_t deferred_fpcr{};
	std::uint32_t deferred_fpsr{};
	bool fp_loaded{true};
	bool fp_dirty{};
	/* Read-only zero slot for translation-specialized production gadgets.
	 * Builder may map an architectural XZR source to this byte offset, avoiding
	 * a runtime register-number comparison. No destination is ever mapped here. */
	std::uint64_t gadget_zero{};
	/* Production may retain the guest FPCR/FPSR in the native registers across
	 * internal translated-block exits.  These private fields preserve the host
	 * environment until the outer guest-dispatch boundary. */
	std::uint64_t gadget_host_fpcr{};
	std::uint64_t gadget_host_fpsr{};
	bool gadget_fp_active{};
	/* Private execution-engine scratch, never guest-visible architectural
	 * state, holding one of two things. An indirect control-flow cache miss
	 * records its data-only cache cell here, with a `continued` event, so the
	 * gadget executor can train that exact site. Every other exit that
	 * writes it records its position in a translated stream instead, and
	 * does so only with an event that does not retire the instruction or
	 * from the FPCR write that enables traps; the executor consumes and
	 * clears that pointer before it trains, so a stream pointer is never
	 * taken for a cache cell. A new exit that records its stream must keep
	 * to that rule. */
	void *gadget_indirect_miss{};
	/* Private gadget-engine return predictor: linked calls push the return
	 * PC with the BlockLink of its block, and a cached return whose site
	 * cells miss pops the newest record when it names the live link
	 * register. Records name link state of one address space, so the
	 * executor empties the ring on every address-space switch. A stale or
	 * mismatched record can only fall back to the ordinary miss path. */
	struct ReturnRecord final {
		/* An unaligned sentinel: no return PC can ever match an empty
		 * record, so an empty ring never follows its null link. */
		std::uint64_t pc{~std::uint64_t{0}};
		const void *link{};
	};
	/* Shared with assembly_offsets.h; a power of two no larger than 256. */
	static constexpr std::size_t return_stack_entries = 16;
	std::array<ReturnRecord, return_stack_entries> gadget_return_stack{};
	std::uint8_t gadget_return_top{};
	/* Private gadget-engine target table of the running address space: a
	 * direct-mapped array of {guest PC, BlockLink} records, indexed by a
	 * fold of the target address, that a cached indirect branch consults
	 * once its site cells -- and for a return the ring -- have missed. The
	 * executor publishes it before every translated run; null turns the
	 * lookup off. Like the ring, a stale or mismatched record can only fall
	 * back to the ordinary miss path. */
	const void *gadget_target_tier{};
	[[nodiscard]] std::uint64_t untag_data_address(
		std::uint64_t address, bool base_sp) const noexcept
	{
		return !base_sp && tbi_enabled ?
			address & 0x00ff'ffff'ffff'ffffULL : address;
	}

	/* Loads a saved context lazily. Its FPCR is masked as an MSR FPCR
	 * write is. */
	void defer_fp_load(const void *source, std::uint32_t source_fpcr,
			  std::uint32_t source_fpsr) noexcept
	{
		deferred_vectors = static_cast<const std::byte *>(source);
		deferred_fpcr = source_fpcr & fpcr_writable_mask;
		deferred_fpsr = source_fpsr;
		fp_loaded = false;
		fp_dirty = false;
	}

	void access_fp_state() noexcept
	{
		if (!fp_loaded) {
			std::memcpy(vectors.data(), deferred_vectors, sizeof(vectors));
			fpcr = deferred_fpcr;
			fpsr = deferred_fpsr;
			fp_loaded = true;
		}
		fp_dirty = true;
	}

	/* The guest FPCR, whether or not the deferred image is loaded yet. */
	[[nodiscard]] std::uint32_t effective_fpcr() const noexcept
	{
		return fp_loaded ? fpcr : deferred_fpcr;
	}

	[[nodiscard]] std::uint64_t read_register(unsigned index,
					     bool stack_pointer) const noexcept
	{
		if (index < registers.size())
			return registers[index];
		return stack_pointer ? sp : 0;
	}

	void write_register(unsigned index, std::uint64_t value,
			    bool stack_pointer) noexcept
	{
		if (index < registers.size())
			registers[index] = value;
		else if (stack_pointer)
			sp = value;
	}
};

} // namespace uml::cpu::arm64
