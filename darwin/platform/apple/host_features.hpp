// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/host_capabilities.hpp"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <sys/sysctl.h>

namespace uml::platform::apple {

using ::uml::platform::ArmFeature;
using ::uml::platform::ArmFeatureSet;
using ::uml::platform::arm_feature_count;
using ::uml::platform::arm_feature_labels;
using ::uml::platform::arm_inventory_feature_count;

enum class HostCapabilityResult : std::uint8_t {
	unsupported,
	supported,
	unavailable,
};

namespace detail {

[[nodiscard]] inline bool
native_jscvt_subnormal_flags_correct() noexcept
{
	constexpr std::uint64_t positive_subnormal = 1;
	constexpr std::array<std::uint64_t, 9> controls{
		0U, 1U, 2U, std::uint64_t{1} << 24U,
		2U | (std::uint64_t{1} << 24U),
		std::uint64_t{1} << 25U, std::uint64_t{1} << 26U,
		4U, 3U << 22U};
	std::uint64_t saved_fpcr = 0;
	std::uint64_t saved_fpsr = 0;
	std::uint64_t saved_nzcv = 0;
	asm volatile(
		"mrs %[saved_fpcr], fpcr\n\t"
		"mrs %[saved_fpsr], fpsr\n\t"
		"mrs %[saved_nzcv], nzcv\n\t"
		: [saved_fpcr] "=&r"(saved_fpcr),
		  [saved_fpsr] "=&r"(saved_fpsr),
		  [saved_nzcv] "=&r"(saved_nzcv)
		:
		: "memory");
	bool correct = true;
	for (const auto control : controls) {
		std::uint64_t flags = 0;
		asm volatile(
			"msr fpcr, %[control]\n\t"
			"msr fpsr, xzr\n\t"
			"msr nzcv, xzr\n\t"
			"fmov d0, %[input]\n\t"
			".inst 0x1e7e0000\n\t"
			"mrs %[flags], nzcv\n\t"
			: [flags] "=r"(flags)
			: [control] "r"(control),
			  [input] "r"(positive_subnormal)
			: "x0", "v0", "cc", "memory");
		correct = correct && (flags & 0xf000'0000U) == 0U;
	}
	asm volatile(
		"msr nzcv, %[saved_nzcv]\n\t"
		"msr fpcr, %[saved_fpcr]\n\t"
		"msr fpsr, %[saved_fpsr]\n\t"
		:
		: [saved_nzcv] "r"(saved_nzcv),
		  [saved_fpcr] "r"(saved_fpcr),
		  [saved_fpsr] "r"(saved_fpsr)
		: "cc", "memory");
	return correct;
}

} // namespace detail

[[nodiscard]] inline HostCapabilityResult query_host_arm_feature_sysctl(
	const char *name) noexcept
{
	int enabled = 0;
	std::size_t size = sizeof(enabled);
	if (sysctlbyname(name, &enabled, &size, nullptr, 0) != 0 ||
	    size != sizeof(enabled))
		return HostCapabilityResult::unavailable;
	return enabled != 0 ? HostCapabilityResult::supported
			    : HostCapabilityResult::unsupported;
}

[[nodiscard]] inline bool query_host_arm_feature(
	const char *name, bool architectural_minimum = false) noexcept
{
	const auto queried = query_host_arm_feature_sysctl(name);
	if (queried != HostCapabilityResult::unavailable)
		return queried == HostCapabilityResult::supported;
	/*
	 * FP and Advanced SIMD are required by Apple's arm64 process ABI.
	 * Sandboxes may deny sysctl while those instructions remain available.
	 * Every optional extension fails closed.
	 */
	return architectural_minimum;
}

[[nodiscard]] inline bool query_host_arm_feature(
	ArmFeature feature) noexcept
{
	if (feature == ArmFeature::floating_point)
		return query_host_arm_feature(
			"hw.optional.floatingpoint", true);
	if (feature == ArmFeature::advanced_simd)
		return query_host_arm_feature("hw.optional.neon", true);

	const auto label =
		arm_feature_labels[static_cast<std::size_t>(feature)];
	std::array<char, 64> name{};
	const int length = std::snprintf(
		name.data(), name.size(), "hw.optional.arm.FEAT_%.*s",
		static_cast<int>(label.size()), label.data());
	if (length <= 0 || static_cast<std::size_t>(length) >= name.size())
		return false;

	return query_host_arm_feature_sysctl(name.data()) ==
		HostCapabilityResult::supported;
}

/* Whether the virtual CPU may implement SME on a host that has the profile.
 * Off by default until both engines implement it completely; setting
 * DARWIN_UML_GUEST_SME to exactly "1" turns it on. It is a run policy, not a
 * test hook, and it never enables anything the host lacks. */
enum class GuestSmePolicy : std::uint8_t {
	off,
	on,
};

inline constexpr const char *guest_sme_policy_variable = "DARWIN_UML_GUEST_SME";

[[nodiscard]] inline GuestSmePolicy parse_guest_sme_policy(
	const char *value) noexcept
{
	return value != nullptr && std::strcmp(value, "1") == 0 ?
		GuestSmePolicy::on : GuestSmePolicy::off;
}

/* Read once, so every engine, the identification registers and the kernel
 * bridge see one answer for the whole process. */
[[nodiscard]] inline GuestSmePolicy guest_sme_policy() noexcept
{
	static const GuestSmePolicy policy =
		parse_guest_sme_policy(std::getenv(guest_sme_policy_variable));
	return policy;
}

/* Whether the virtual CPU may implement non-streaming SVE. Off by default
 * until both engines implement it completely; setting DARWIN_UML_GUEST_SVE
 * to exactly "1" turns it on. Like the guest-SME policy it is a run policy,
 * not a test hook, and an engine still presents SVE only once it implements
 * all of it (cpu/arm64/sve_profile.hpp). */
enum class GuestSvePolicy : std::uint8_t {
	off,
	on,
};

inline constexpr const char *guest_sve_policy_variable = "DARWIN_UML_GUEST_SVE";

[[nodiscard]] inline GuestSvePolicy parse_guest_sve_policy(
	const char *value) noexcept
{
	return value != nullptr && std::strcmp(value, "1") == 0 ?
		GuestSvePolicy::on : GuestSvePolicy::off;
}

/* Read once, as the guest-SME policy is. */
[[nodiscard]] inline GuestSvePolicy guest_sve_policy() noexcept
{
	static const GuestSvePolicy policy =
		parse_guest_sve_policy(std::getenv(guest_sve_policy_variable));
	return policy;
}

/* Whether the virtual CPU may run AArch32 (A32 and T32) EL0 tasks. Off by
 * default until both engines implement the profile completely and the
 * default is turned on; setting DARWIN_UML_GUEST_AARCH32 to exactly "1"
 * turns it on for development. Unlike the SME and SVE policies it adds no
 * host token: AArch32 uses the A64 features the host already reports, so the
 * policy and each engine's level (cpu/arm64/aarch32/a32_profile.hpp,
 * a32_presented_support) are all that decide what a run presents. */
enum class GuestAarch32Policy : std::uint8_t {
	off,
	on,
};

inline constexpr const char *guest_aarch32_policy_variable =
	"DARWIN_UML_GUEST_AARCH32";

[[nodiscard]] inline GuestAarch32Policy parse_guest_aarch32_policy(
	const char *value) noexcept
{
	return value != nullptr && std::strcmp(value, "1") == 0 ?
		GuestAarch32Policy::on : GuestAarch32Policy::off;
}

/* Read once, as the guest-SME and guest-SVE policies are. */
[[nodiscard]] inline GuestAarch32Policy guest_aarch32_policy() noexcept
{
	static const GuestAarch32Policy policy = parse_guest_aarch32_policy(
		std::getenv(guest_aarch32_policy_variable));
	return policy;
}

/* The SVE tokens, which only the guest-SVE policy lets the prober report. */
inline constexpr std::array sve_policy_features{ArmFeature::sve,
	ArmFeature::sve2, ArmFeature::sve_aes, ArmFeature::sve_pmull128,
	ArmFeature::sve_sha3};

/* `host` with the SVE tokens the policy gives it. The engines lower SVE onto
 * the host's AdvSIMD forms, so each part needs the host features its
 * lowering uses: SVE and SVE2 need FP16 and the dot product, which every
 * supported host has; SVE_AES and SVE_PMULL128 come together, as
 * ID_AA64ZFR0_EL1.AES reports both, and need AES and PMULL; SVE_SHA3 needs
 * SHA3. With the policy off, or on a host without FP16 or the dot product,
 * no SVE token is reported. Any SVE token the host's own sysctls claimed is
 * replaced, since the virtual CPU's profile is not the host's. */
[[nodiscard]] inline ::uml::platform::HostCapabilities apply_guest_sve_policy(
	::uml::platform::HostCapabilities host, GuestSvePolicy policy) noexcept
{
	const auto set = [&host](ArmFeature feature, bool on) noexcept {
		host.supported.set(static_cast<std::size_t>(feature), on);
	};
	const bool sve = policy == GuestSvePolicy::on &&
		host.has(ArmFeature::fp16) && host.has(ArmFeature::dotprod);
	const bool aes = sve && host.has(ArmFeature::aes) &&
		host.has(ArmFeature::pmull);
	set(ArmFeature::sve, sve);
	set(ArmFeature::sve2, sve);
	set(ArmFeature::sve_aes, aes);
	set(ArmFeature::sve_pmull128, aes);
	set(ArmFeature::sve_sha3, sve && host.has(ArmFeature::sha3));
	return host;
}

/* hw.optional.arm.sme_max_svl_b: the host's streaming vector length in
 * bytes. Zero when the sysctl is missing, has an unexpected size, or holds
 * a value no SME implementation can have (a power of two from 16 to 256). */
[[nodiscard]] inline std::uint16_t query_host_sme_svl_bytes() noexcept
{
	std::uint64_t value = 0;
	std::size_t size = sizeof(value);
	/* An int today; either width fills the low bytes of the zeroed
	 * little-endian value. */
	if (sysctlbyname("hw.optional.arm.sme_max_svl_b", &value, &size,
		    nullptr, 0) != 0 ||
	    (size != sizeof(std::uint32_t) && size != sizeof(value)))
		return 0;
	if (value < 16 || value > 256 || (value & (value - 1)) != 0)
		return 0;
	return static_cast<std::uint16_t>(value);
}

/* The SME tokens, which only the guest-SME policy lets the prober report. */
inline constexpr std::array sme_policy_features{ArmFeature::sme,
	ArmFeature::sme2, ArmFeature::sme_f64f64, ArmFeature::sme_i16i64};

/* Platform probing ends here. Runtime copies this immutable neutral profile
 * into EngineConfiguration; CPU translation code never depends on sysctl or
 * any Apple header. With the guest-SME policy off, or without a valid
 * streaming vector length, no SME token is reported and sme_svl_bytes is 0;
 * sme_hardware reports FEAT_SME either way. The SVE tokens follow the
 * guest-SVE policy (apply_guest_sve_policy).
 */
[[nodiscard]] inline ::uml::platform::HostCapabilities probe_host_capabilities(
	GuestSmePolicy sme_policy,
	GuestSvePolicy sve_policy = GuestSvePolicy::off) noexcept
{
	::uml::platform::HostCapabilities result;
	for (std::size_t index = 0; index < arm_feature_count; ++index)
		result.supported.set(index, query_host_arm_feature(
			static_cast<ArmFeature>(index)));
	result.floating_point = result.has(ArmFeature::floating_point);
	result.advanced_simd = result.has(ArmFeature::advanced_simd);
	result.fp16 = result.has(ArmFeature::fp16);
	result.cssc = result.has(ArmFeature::cssc);
	result.aes = result.has(ArmFeature::aes);
	result.pmull = result.has(ArmFeature::pmull);
	result.sha1 = result.has(ArmFeature::sha1);
	result.sha2 = result.has(ArmFeature::sha256);
	result.crc32 = result.has(ArmFeature::crc32);
	result.sha3 = result.has(ArmFeature::sha3);
	result.sha512 = result.has(ArmFeature::sha512);
	if (result.has(ArmFeature::jscvt))
		result.jscvt_subnormal_flags_correct =
			detail::native_jscvt_subnormal_flags_correct();
	result.sme_hardware = result.has(ArmFeature::sme);
	if (sme_policy == GuestSmePolicy::on && result.sme_hardware)
		result.sme_svl_bytes = query_host_sme_svl_bytes();
	if (result.sme_svl_bytes == 0)
		for (const auto feature : sme_policy_features)
			result.supported.reset(static_cast<std::size_t>(feature));
	return apply_guest_sve_policy(result, sve_policy);
}

[[nodiscard]] inline const ::uml::platform::HostCapabilities &
probed_host_capabilities() noexcept
{
	static const ::uml::platform::HostCapabilities features =
		probe_host_capabilities(guest_sme_policy(), guest_sve_policy());
	return features;
}

} // namespace uml::platform::apple
