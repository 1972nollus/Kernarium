// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/host_capabilities.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64 {

/* The non-streaming SVE profile of the virtual CPU and the state it adds,
 * shared by both engines, the runtime's state transfer and the tests.
 *
 * The profile is FEAT_SVE with FEAT_SVE2, plus FEAT_SVE_AES with
 * FEAT_SVE_PMULL128, FEAT_SVE_SHA3, and the SVE forms of FEAT_I8MM and
 * FEAT_BF16, each where the host has the AdvSIMD forms the engines lower it
 * onto. The prober reports the tokens only under the guest-SVE policy
 * (platform/apple/host_features.hpp), so this is the policy too. No
 * supported host implements SVE: outside streaming mode both engines lower
 * every SVE word onto the host's AdvSIMD forms, so the vector length (VL) is
 * one fixed value on every host, independent of the streaming vector length
 * of the SME profile. At that VL Z_n is exactly V_n, which CpuState::vectors
 * already holds; the state SVE adds is P0-P15 and FFR. */
inline constexpr unsigned sve_vector_length = 16U;
inline constexpr unsigned sve_p_registers = 16U;

/* How far an engine implements the profile outside streaming mode: nothing
 * yet (every non-streaming SVE word is the illegal-instruction exit), the
 * control core, or every form of the profile. An engine presents the
 * profile to its guest only when complete (sve_host_for_support). */
enum class SveSupport : std::uint8_t {
	none,
	control,
	complete,
};

/* The SVE tokens, in the order of ArmFeature. */
inline constexpr std::array sve_features{ArmFeature::sve, ArmFeature::sve2,
	ArmFeature::sve_aes, ArmFeature::sve_pmull128, ArmFeature::sve_sha3};

/* Whether the virtual CPU implements SVE on `host`. The profile has no SVE
 * without SVE2. */
[[nodiscard]] inline bool implements_sve(const HostCapabilities &host) noexcept
{
	return host.has(ArmFeature::sve) && host.has(ArmFeature::sve2);
}

/* The host an engine at `support` presents the virtual CPU for: `host`
 * less every SVE token below complete, so its identification registers and
 * the guest's HWCAPs, which derive from the profile, advertise no SVE form
 * the engine would take as illegal. A host without SVE, which includes every
 * host while the guest-SVE policy is off, comes back unchanged. */
[[nodiscard]] inline HostCapabilities sve_host_for_support(
	HostCapabilities host, SveSupport support) noexcept
{
	if (support < SveSupport::complete)
		for (const auto feature : sve_features)
			host.supported.reset(static_cast<std::size_t>(feature));
	return host;
}

/* A non-streaming predicate in the engines' mask form: byte k is 0xff where
 * bit k of the architectural predicate is set and 0x00 where it is clear.
 * At this VL a predicate has one bit per byte of a vector, 16 bits, so the
 * image maps one to one onto the bits and serves every element size as a
 * lane mask. The 16-bit packed form exists only at the bridge, in signal
 * frames and in ptrace. */
using SvePredicate = std::array<std::uint8_t, sve_vector_length>;

[[nodiscard]] constexpr SvePredicate sve_predicate_mask(
	std::uint16_t packed) noexcept
{
	SvePredicate mask{};
	for (unsigned bit = 0; bit < sve_vector_length; ++bit)
		mask[bit] = (packed >> bit & 1U) != 0U ? 0xffU : 0x00U;
	return mask;
}

/* The packed form of a mask; a byte counts as set by its top bit, which is
 * the bit an engine's lane mask tests. */
[[nodiscard]] constexpr std::uint16_t sve_predicate_packed(
	const SvePredicate &mask) noexcept
{
	unsigned packed = 0;
	for (unsigned bit = 0; bit < sve_vector_length; ++bit)
		packed |= static_cast<unsigned>(mask[bit] >> 7U) << bit;
	return static_cast<std::uint16_t>(packed);
}

/* The mask with each element's lowest byte set, for elements of `esize`
 * bytes (1, 2, 4 or 8). A predicate written for that element size holds
 * only these bits, a consumer of it reads only them, and the all-true
 * predicate PTRUE writes for that size is this mask. */
[[nodiscard]] constexpr SvePredicate sve_element_pattern(
	unsigned esize) noexcept
{
	SvePredicate mask{};
	for (unsigned byte = 0; byte < sve_vector_length; byte += esize)
		mask[byte] = 0xffU;
	return mask;
}

/* NZCV, as CpuState::nzcv holds it, that PredTest() leaves for a predicate
 * whose first `active` of `elements` elements are active, governed by the
 * all-true predicate: N, the first element is active; Z, none is; C, the
 * last is not; V clear. A WHILE* result has exactly this shape. */
[[nodiscard]] constexpr std::uint32_t sve_leading_predicate_nzcv(
	unsigned active, unsigned elements) noexcept
{
	const std::uint32_t n = active != 0U ? 1U : 0U;
	const std::uint32_t z = active == 0U ? 1U : 0U;
	const std::uint32_t c = active < elements ? 1U : 0U;
	return n << 31U | z << 30U | c << 29U;
}

/* What a WHILE* instruction that leaves `active` elements of `esize` bytes
 * active writes: the predicate, in mask form, and NZCV. `active` is at
 * most sve_vector_length / esize. */
struct SveLeadingPredicate final {
	SvePredicate predicate;
	std::uint32_t nzcv;
};

[[nodiscard]] constexpr SveLeadingPredicate sve_leading_predicate(
	unsigned esize, unsigned active) noexcept
{
	const unsigned elements = sve_vector_length / esize;
	SveLeadingPredicate result{{}, sve_leading_predicate_nzcv(active,
		elements)};
	for (unsigned element = 0; element < active && element < elements;
	     ++element)
		result.predicate[element * esize] = 0xffU;
	return result;
}

/* The table an engine indexes by the active count, for one element size:
 * entry n leaves the first n elements active. */
template <unsigned Esize>
[[nodiscard]] constexpr auto sve_leading_predicate_table() noexcept
{
	constexpr unsigned elements = sve_vector_length / Esize;
	std::array<SveLeadingPredicate, elements + 1U> table{};
	for (unsigned active = 0; active <= elements; ++active)
		table[active] = sve_leading_predicate(Esize, active);
	return table;
}

static_assert(sve_predicate_packed(sve_predicate_mask(0xa5c3U)) == 0xa5c3U);
static_assert(sve_predicate_packed(sve_element_pattern(1)) == 0xffffU);
static_assert(sve_predicate_packed(sve_element_pattern(2)) == 0x5555U);
static_assert(sve_predicate_packed(sve_element_pattern(4)) == 0x1111U);
static_assert(sve_predicate_packed(sve_element_pattern(8)) == 0x0101U);
static_assert(sve_leading_predicate_table<8>()[2].predicate ==
	sve_element_pattern(8));
static_assert(sve_leading_predicate_table<1>()[0].nzcv == 0x6000'0000U);
static_assert(sve_leading_predicate_table<4>()[4].nzcv == 0x8000'0000U);
static_assert(sve_leading_predicate_table<2>()[3].nzcv == 0xa000'0000U);

} // namespace uml::cpu::arm64
