// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/host_capabilities.hpp"

#include <array>
#include <bitset>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <string_view>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

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

using HostInstructionProbe = void (*)() noexcept;

namespace detail {

inline constexpr int host_probe_unsupported_exit = 120;
inline constexpr int host_probe_setup_failed_exit = 121;

[[noreturn]] inline void host_probe_sigill_handler(int) noexcept
{
	::_exit(host_probe_unsupported_exit);
}

inline void probe_lor_instruction() noexcept
{
#if defined(__APPLE__) && defined(__aarch64__)
	alignas(std::uint64_t) std::uint64_t value = 0;
	const auto *address = &value;
	asm volatile(
		"mov x1, %0\n\t"
		".inst 0xc8df7c20"
		:
		: "r"(address)
		: "x0", "x1", "memory");
#endif
}

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

[[nodiscard]] inline HostInstructionProbe instruction_probe_for(
	ArmFeature feature) noexcept
{
	switch (feature) {
	case ArmFeature::lor:
		return probe_lor_instruction;
	default:
		return nullptr;
	}
}

} // namespace detail

/*
 * Run optional-instruction probes in a child process.  The child installs its
 * own SIGILL disposition and mask, so the caller's process-wide handler and
 * the calling thread's signal mask remain untouched even when the probe
 * faults.  Any setup/process failure is reported as unavailable and therefore
 * remains fail-closed.
 */
[[nodiscard]] inline HostCapabilityResult probe_host_instruction(
	HostInstructionProbe probe) noexcept
{
#if defined(__APPLE__) && defined(__aarch64__)
	if (probe == nullptr)
		return HostCapabilityResult::unavailable;

	const pid_t child = ::fork();
	if (child < 0)
		return HostCapabilityResult::unavailable;
	if (child == 0) {
		struct sigaction action {};
		action.sa_handler = detail::host_probe_sigill_handler;
		if (sigemptyset(&action.sa_mask) != 0 ||
		    ::sigaction(SIGILL, &action, nullptr) != 0)
			::_exit(detail::host_probe_setup_failed_exit);

		sigset_t mask;
		if (sigemptyset(&mask) != 0 ||
		    sigaddset(&mask, SIGILL) != 0 ||
		    ::sigprocmask(SIG_UNBLOCK, &mask, nullptr) != 0)
			::_exit(detail::host_probe_setup_failed_exit);

		probe();
		::_exit(0);
	}

	int status = 0;
	pid_t waited;
	do {
		waited = ::waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited != child)
		return HostCapabilityResult::unavailable;
	if (WIFEXITED(status)) {
		const int code = WEXITSTATUS(status);
		if (code == 0)
			return HostCapabilityResult::supported;
		if (code == detail::host_probe_unsupported_exit)
			return HostCapabilityResult::unsupported;
		return HostCapabilityResult::unavailable;
	}
	if (WIFSIGNALED(status) && WTERMSIG(status) == SIGILL)
		return HostCapabilityResult::unsupported;
	return HostCapabilityResult::unavailable;
#else
	(void)probe;
	return HostCapabilityResult::unavailable;
#endif
}

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

/*
 * Apple spells these sysctls with the architecture's own capitalization -
 * `FEAT_DotProd`, `FEAT_FlagM`, `FEAT_PAuth`, `FEAT_WFxT` - while this
 * project's labels are lowercase with underscores. `sysctlbyname` is
 * case-sensitive, so deriving the name from the label alone queried
 * `FEAT_dotprod` and missed every single one: no feature was ever answered by
 * sysctl, and detection silently fell through to the SIGILL instruction probe.
 * A feature with no probe then reported unsupported on a host that has it.
 *
 * Only the spellings that differ need an entry; anything absent keeps the
 * derived name and, failing that, the probe.
 */
struct ArmFeatureSysctlName final {
	ArmFeature feature;
	std::string_view name;
};

inline constexpr std::array arm_feature_sysctl_names{
	ArmFeatureSysctlName{ArmFeature::aes, "AES"},
	ArmFeatureSysctlName{ArmFeature::afp, "AFP"},
	ArmFeatureSysctlName{ArmFeature::bf16, "BF16"},
	ArmFeatureSysctlName{ArmFeature::bti, "BTI"},
	ArmFeatureSysctlName{ArmFeature::crc32, "CRC32"},
	ArmFeatureSysctlName{ArmFeature::cssc, "CSSC"},
	ArmFeatureSysctlName{ArmFeature::dit, "DIT"},
	ArmFeatureSysctlName{ArmFeature::dotprod, "DotProd"},
	ArmFeatureSysctlName{ArmFeature::dpb, "DPB"},
	ArmFeatureSysctlName{ArmFeature::dpb2, "DPB2"},
	ArmFeatureSysctlName{ArmFeature::ebf16, "EBF16"},
	ArmFeatureSysctlName{ArmFeature::fcma, "FCMA"},
	ArmFeatureSysctlName{ArmFeature::fhm, "FHM"},
	ArmFeatureSysctlName{ArmFeature::flag_m, "FlagM"},
	ArmFeatureSysctlName{ArmFeature::flag_m2, "FlagM2"},
	ArmFeatureSysctlName{ArmFeature::fp16, "FP16"},
	ArmFeatureSysctlName{ArmFeature::frintts, "FRINTTS"},
	ArmFeatureSysctlName{ArmFeature::hbc, "HBC"},
	ArmFeatureSysctlName{ArmFeature::i8mm, "I8MM"},
	ArmFeatureSysctlName{ArmFeature::jscvt, "JSCVT"},
	ArmFeatureSysctlName{ArmFeature::lrcpc, "LRCPC"},
	ArmFeatureSysctlName{ArmFeature::lrcpc2, "LRCPC2"},
	ArmFeatureSysctlName{ArmFeature::lse, "LSE"},
	ArmFeatureSysctlName{ArmFeature::mte, "MTE"},
	ArmFeatureSysctlName{ArmFeature::mte2, "MTE2"},
	ArmFeatureSysctlName{ArmFeature::pauth, "PAuth"},
	ArmFeatureSysctlName{ArmFeature::pmull, "PMULL"},
	ArmFeatureSysctlName{ArmFeature::rdm, "RDM"},
	ArmFeatureSysctlName{ArmFeature::sb, "SB"},
	ArmFeatureSysctlName{ArmFeature::sha1, "SHA1"},
	ArmFeatureSysctlName{ArmFeature::sha256, "SHA256"},
	ArmFeatureSysctlName{ArmFeature::sha3, "SHA3"},
	ArmFeatureSysctlName{ArmFeature::sha512, "SHA512"},
	ArmFeatureSysctlName{ArmFeature::sme, "SME"},
	ArmFeatureSysctlName{ArmFeature::specres, "SPECRES"},
	ArmFeatureSysctlName{ArmFeature::specres2, "SPECRES2"},
	ArmFeatureSysctlName{ArmFeature::ssbs, "SSBS"},
	ArmFeatureSysctlName{ArmFeature::wfxt, "WFxT"},
};

[[nodiscard]] inline constexpr std::string_view arm_feature_sysctl_name(
	ArmFeature feature) noexcept
{
	for (const auto &entry : arm_feature_sysctl_names)
		if (entry.feature == feature)
			return entry.name;
	return {};
}

[[nodiscard]] inline bool query_host_arm_feature(
	ArmFeature feature) noexcept
{
	if (feature == ArmFeature::floating_point)
		return query_host_arm_feature(
			"hw.optional.floatingpoint", true);
	if (feature == ArmFeature::advanced_simd)
		return query_host_arm_feature("hw.optional.neon", true);

	const auto exact = arm_feature_sysctl_name(feature);
	const auto label = exact.empty() ?
		arm_feature_labels[static_cast<std::size_t>(feature)] : exact;
	std::array<char, 64> name{};
	const int length = std::snprintf(
		name.data(), name.size(), "hw.optional.arm.FEAT_%.*s",
		static_cast<int>(label.size()), label.data());
	if (length <= 0 || static_cast<std::size_t>(length) >= name.size())
		return false;

	const auto queried = query_host_arm_feature_sysctl(name.data());
	if (queried != HostCapabilityResult::unavailable)
		return queried == HostCapabilityResult::supported;

	const auto probe = detail::instruction_probe_for(feature);
	return probe != nullptr &&
		probe_host_instruction(probe) == HostCapabilityResult::supported;
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
		result.afp = result.has(ArmFeature::afp);
		if (result.has(ArmFeature::jscvt))
			result.jscvt_subnormal_flags_correct =
				detail::native_jscvt_subnormal_flags_correct();
		return result;
	}();
	return features;
}

} // namespace uml::platform::apple
