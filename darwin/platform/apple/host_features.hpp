// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/host_capabilities.hpp"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
#if defined(__APPLE__) && defined(__aarch64__)
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
#else
	return false;
#endif
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
#if defined(__APPLE__) && defined(__aarch64__)
	return architectural_minimum;
#else
	(void)architectural_minimum;
	return false;
#endif
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

/* Platform probing ends here. Runtime copies this immutable neutral profile
 * into EngineConfiguration; CPU translation code never depends on sysctl or
 * any Apple header.
 */
[[nodiscard]] inline const ::uml::platform::HostCapabilities &
probed_host_capabilities() noexcept
{
	static const ::uml::platform::HostCapabilities features = [] {
		::uml::platform::HostCapabilities result;
		for (std::size_t index = 0; index < arm_feature_count; ++index)
			result.supported.set(index, query_host_arm_feature(
				static_cast<ArmFeature>(index)));
		result.floating_point =
			result.has(ArmFeature::floating_point);
		result.advanced_simd =
			result.has(ArmFeature::advanced_simd);
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
		return result;
	}();
	return features;
}

} // namespace uml::platform::apple
