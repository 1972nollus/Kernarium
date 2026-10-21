// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/decode_types.hpp"
#include "cpu/host_capabilities.hpp"

#include <cstdint>

namespace uml::cpu::arm64 {

/* The AArch32 EL0 profile of the virtual CPU, shared by both engines, the
 * runtime's state transfer and the tests (docs/architecture.md).
 *
 * The profile is Armv8-A AArch32 at EL0: the A32 and T32 instruction sets
 * with interworking and IT, the Armv8 load-acquire and store-release group,
 * VFPv4-D32 and Advanced SIMD, the Armv8 floating-point additions (VSEL,
 * VMAXNM, VRINT and VCVT with a rounding mode), and, each where the host has
 * the A64 feature the engines lower it onto, CRC32, AES, PMULL, SHA1, SHA2,
 * FP16 arithmetic, the dot product, FHM, BF16, I8MM, SB, RDM, FCMA and
 * JSCVT. It has no SWP, SETEND, short vectors, floating-point trap enables
 * or ThumbEE, and every encoding outside it is UNDEFINED. */

/* Which optional parts of the profile a guest sees on a host. Each field is
 * the A64 host predicate its lowering uses. The AArch32 identification
 * registers (detail::a32_id_register_value) and the compat HWCAPs derive
 * from this and from nothing else. The integer core, the Armv8 load-acquire
 * and store-release group (which arm64 Linux's compat kuser helpers and
 * vDSO may execute), VFPv4-D32, Advanced SIMD and the Armv8 floating-point
 * additions every host supports are always shown. */
struct A32Features final {
	bool crc32{};
	bool aes{};
	bool pmull{};
	bool sha1{};
	bool sha2{};
	bool fp16{};
	bool dotprod{};
	bool fhm{};
	bool bf16{};
	bool i8mm{};
	bool sb{};
	bool rdm{};
	bool fcma{};
	bool jscvt{};

	[[nodiscard]] bool operator==(const A32Features &) const noexcept = default;
};

[[nodiscard]] inline A32Features a32_features(
	const HostCapabilities &host) noexcept
{
	A32Features features;
	features.crc32 = host.crc32;
	features.aes = host.aes;
	features.pmull = host.aes && host.pmull;
	features.sha1 = host.sha1;
	features.sha2 = host.sha2;
	features.fp16 = host.fp16;
	features.dotprod = host.has(ArmFeature::dotprod);
	features.fhm = host.has(ArmFeature::fhm);
	features.bf16 = host.has(ArmFeature::bf16);
	features.i8mm = host.has(ArmFeature::i8mm);
	features.sb = host.has(ArmFeature::sb);
	features.rdm = host.has(ArmFeature::rdm);
	features.fcma = host.has(ArmFeature::fcma);
	features.jscvt = host.has(ArmFeature::jscvt);
	return features;
}

/* The profile an engine decodes AArch32 against on this host: exactly the
 * optional extensions the guest is shown (a32_features). A form of an
 * extension the host lacks is therefore UNDEFINED, as the HWCAPs and the
 * identification registers say, and an engine never executes it, whether it
 * lowers the extension or runs it in the semantic library. */
[[nodiscard]] inline aarch32::Profile a32_decode_profile(
	const HostCapabilities &host) noexcept
{
	const auto shown = a32_features(host);
	aarch32::FeatureSet features;
	const auto add = [&features](bool present,
				     aarch32::Feature feature) noexcept {
		if (present)
			features = features.with(feature);
	};
	add(shown.fp16, aarch32::Feature::fp16);
	add(shown.aes, aarch32::Feature::aes);
	add(shown.pmull, aarch32::Feature::pmull);
	add(shown.sha1, aarch32::Feature::sha1);
	add(shown.sha2, aarch32::Feature::sha2);
	add(shown.crc32, aarch32::Feature::crc32);
	add(shown.dotprod, aarch32::Feature::dotprod);
	add(shown.fhm, aarch32::Feature::fhm);
	add(shown.bf16, aarch32::Feature::bf16);
	add(shown.i8mm, aarch32::Feature::i8mm);
	add(shown.sb, aarch32::Feature::sb);
	add(shown.rdm, aarch32::Feature::rdm);
	add(shown.fcma, aarch32::Feature::fcma);
	add(shown.jscvt, aarch32::Feature::jscvt);
	return aarch32::Profile{features};
}

} // namespace uml::cpu::arm64
