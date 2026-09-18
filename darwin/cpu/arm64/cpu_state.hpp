// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace uml::cpu::arm64 {

struct alignas(16) VectorRegister final {
	std::array<std::byte, 16> bytes{};
};

struct PointerAuthKey final {
	std::uint64_t low{};
	std::uint64_t high{};

	[[nodiscard]] constexpr bool operator==(
		const PointerAuthKey &) const noexcept = default;
};

inline constexpr std::array<PointerAuthKey, 5>
	deterministic_pointer_auth_keys{{
		{0x84be'85ce'9804'e94bULL, 0xec28'0d07'4b7a'c6d3ULL},
		{0x4a65'7b64'56cb'c3d1ULL, 0x2b7e'1516'28ae'd2a6ULL},
		{0x09cf'4f3c'abf7'1528ULL, 0x7f4a'7c15'9e37'79b9ULL},
		{0xd1b5'4a32'd192'ed03ULL, 0x94d0'49bb'1331'11ebULL},
		{0x2545'f491'4f6c'dd1dULL, 0xbf58'476d'1ce4'e5b9ULL},
	}};

struct CpuState final {
	std::array<std::uint64_t, 31> registers{};
	std::uint64_t sp{};
	std::uint64_t pc{};
	std::uint32_t nzcv{};
	std::array<VectorRegister, 32> vectors{};
	std::uint32_t fpcr{};
	std::uint32_t fpsr{};
	std::uint64_t tls{};
	/* EL0's effective MTE state, independent TBI0 address state, and the
	 * GCR_EL1.Exclude value supplied by the userspace runtime. MTE implies
	 * tagged addresses, while Android also uses top-byte-ignore without MTE
	 * allocation-tag checks. Production ADDG/SUBG gadgets use the derived
	 * table so tag selection is a single indexed load with no hot-path loop.
	 * Change the MTE fields only through set_mte_configuration().
	 */
	bool mte_enabled{};
	bool tbi_enabled{};
	std::uint16_t mte_exclude{};
	std::array<std::uint8_t, 256> mte_tag_choices{};
	bool mte_random_tags{};
	std::uint16_t mte_seed{1};
	std::uint8_t mte_tag{};
	std::uint64_t mte_random_state{0x9e37'79b9'7f4a'7c15ULL};
	/* Effective CPTA and CPTM controls. Addition overflow and signed
	 * multiplication overflow are independently configurable architecturally.
	 */
	bool cpa_enabled{};
	bool cpa_multiply_enabled{};
	std::uint32_t accdata{};
	/* EL0-visible PSTATE fields controlled by MSR (immediate).  DAIF access is
	 * explicitly gated because SCTLR_EL1.UMA determines whether userspace may
	 * use DAIFSet/DAIFClr; the other fields are effective feature state. */
	bool pstate_ssbs{};
	bool pstate_dit{};
	bool pstate_tco{};
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
	/* FEAT_FPMR state is kept separately from the lazily imported legacy
	 * FPCR/FPSR/vector image.  Its reset value selects E5M2 with zero scales. */
	std::uint64_t fpmr{};
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
	/* Private execution-engine scratch. An indirect control-flow cache miss
	 * records its data-only cache cell here so the gadget executor can train
	 * that exact site. This is never guest-visible architectural state. */
	void *gadget_indirect_miss{};
	/* JIT blocks using the optional native mode register retain the host value
	 * here so returning and terminal C++ helpers can run in the host-owned
	 * environment without losing the live guest value. */
	std::uint64_t gadget_host_fpmr{};
	bool gadget_fpmr_active{};
	/* Effective EL0 Guarded Control Stack state.  The runtime does not
	 * advertise FEAT_GCS yet, so production frames retain the architectural
	 * reset values.  Conformance tests enable the feature explicitly. */
	std::uint64_t gcs_pointer{};
	bool gcs_implemented{};
	bool gcs_push_enabled{};
	bool gcs_store_enabled{};
	/*
	 * Five guest-owned 128-bit pointer-authentication keys: instruction A/B,
	 * data A/B, and generic. Production imports these from the task register
	 * frame, so signatures survive syscalls, migration, and vCPU changes
	 * without ever exposing Darwin's process keys. Direct engine tests retain
	 * deterministic defaults and may replace them with explicit test keys.
	 */
	std::array<PointerAuthKey, 5> pointer_auth_keys{
		deterministic_pointer_auth_keys};
	/*
	 * HINT-space key operations are architectural no-ops when the base
	 * extension is not part of the virtual CPU profile.  Unit tests leave
	 * the complete software implementation enabled; production selects the
	 * leaner guest profile before entering any execution engine.
	 */
	bool pointer_authentication_enabled{true};
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
	[[nodiscard]] bool tagged_addresses_enabled() const noexcept
	{
		return mte_enabled || tbi_enabled;
	}
	[[nodiscard]] std::uint64_t untag_data_address(
		std::uint64_t address, bool base_sp) const noexcept
	{
		return !base_sp && tagged_addresses_enabled() ?
			address & 0x00ff'ffff'ffff'ffffULL : address;
	}

	[[nodiscard]] std::uint8_t choose_mte_tag(
		unsigned start, unsigned offset) const noexcept
	{
		if (!mte_enabled || mte_exclude == 0xffffU)
			return 0;
		unsigned tag = start & 15U;
		unsigned remaining = offset & 15U;
		if (remaining == 0) {
			while ((mte_exclude & (1U << tag)) != 0)
				tag = (tag + 1U) & 15U;
		} else {
			while (remaining-- != 0) {
				tag = (tag + 1U) & 15U;
				while ((mte_exclude & (1U << tag)) != 0)
					tag = (tag + 1U) & 15U;
			}
		}
		return static_cast<std::uint8_t>(tag);
	}

	void set_mte_configuration(bool enabled, std::uint16_t exclude) noexcept
	{
		mte_enabled = enabled;
		mte_exclude = exclude;
		for (unsigned start = 0; start < 16; ++start)
			for (unsigned offset = 0; offset < 16; ++offset)
				mte_tag_choices[start * 16 + offset] =
					choose_mte_tag(start, offset);
	}

	void set_mte_generator(bool random_tags, std::uint16_t seed,
		std::uint8_t tag, std::uint64_t random_state =
			0x9e37'79b9'7f4a'7c15ULL) noexcept
	{
		mte_random_tags = random_tags;
		mte_seed = seed;
		mte_tag = tag & 15U;
		mte_random_state = random_state;
	}

	void defer_fp_load(const void *source, std::uint32_t source_fpcr,
			  std::uint32_t source_fpsr) noexcept
	{
		deferred_vectors = static_cast<const std::byte *>(source);
		deferred_fpcr = source_fpcr;
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
