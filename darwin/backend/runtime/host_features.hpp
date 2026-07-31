// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

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

namespace uml::backend {

enum class ArmFeature : std::size_t {
	aes,
	asmv8p2,
	ats1a,
	advanced_simd,
	bf16,
	brbe,
	bti,
	chk,
	clrbhb,
	cmh,
	cmpbr,
	cpa,
	crc32,
	cssc,
	d128,
	dgh,
	dit,
	dpb,
	dpb2,
	dotprod,
	e3dse,
	ebep,
	ebf16,
	f16f32dot,
	f16f32mm,
	f16mm,
	f8f16mm,
	f8f32mm,
	faminmax,
	fcma,
	fhm,
	floating_point,
	fp16,
	fp8,
	fp8dot2,
	fp8dot4,
	fp8fma,
	fprcvt,
	frintts,
	flag_m,
	flag_m2,
	gcie,
	gcs,
	hbc,
	hinte,
	i8mm,
	ite,
	jscvt,
	lor,
	lrcpc,
	lrcpc2,
	lrcpc3,
	ls64,
	ls64_accdata,
	ls64_v,
	lscp,
	lse,
	lse128,
	lsfe,
	lsui,
	lut,
	mec,
	mops,
	mte,
	mte2,
	mtetc,
	nmi,
	occmo,
	pan,
	pan2,
	pauth,
	pauth_lr,
	pcdphint,
	pmull,
	prfmslc,
	pops,
	ras,
	rdm,
	rme,
	rme_gpc3,
	rprfm,
	sb,
	sha1,
	sha256,
	sha3,
	sha512,
	sm3,
	sm4,
	sme,
	spe,
	specres,
	specres2,
	spe_exc,
	ssbs,
	sysinstr128,
	sysreg128,
	the,
	tlbid,
	tlbios,
	tlbirange,
	tlbiw,
	trbe_exc,
	trf,
	uao,
	vhe,
	wfxt,
	xs,
	inventory_count,
	afp = inventory_count,
	count,
};

inline constexpr std::array arm_feature_labels{
	std::string_view{"AES"}, std::string_view{"ASMv8p2"},
	std::string_view{"ATS1A"}, std::string_view{"AdvSIMD"},
	std::string_view{"BF16"}, std::string_view{"BRBE"},
	std::string_view{"BTI"}, std::string_view{"CHK"},
	std::string_view{"CLRBHB"}, std::string_view{"CMH"},
	std::string_view{"CMPBR"}, std::string_view{"CPA"},
	std::string_view{"CRC32"}, std::string_view{"CSSC"},
	std::string_view{"D128"}, std::string_view{"DGH"},
	std::string_view{"DIT"}, std::string_view{"DPB"},
	std::string_view{"DPB2"}, std::string_view{"DotProd"},
	std::string_view{"E3DSE"}, std::string_view{"EBEP"},
	std::string_view{"EBF16"}, std::string_view{"F16F32DOT"},
	std::string_view{"F16F32MM"}, std::string_view{"F16MM"},
	std::string_view{"F8F16MM"}, std::string_view{"F8F32MM"},
	std::string_view{"FAMINMAX"}, std::string_view{"FCMA"},
	std::string_view{"FHM"}, std::string_view{"FP"},
	std::string_view{"FP16"}, std::string_view{"FP8"},
	std::string_view{"FP8DOT2"}, std::string_view{"FP8DOT4"},
	std::string_view{"FP8FMA"}, std::string_view{"FPRCVT"},
	std::string_view{"FRINTTS"}, std::string_view{"FlagM"},
	std::string_view{"FlagM2"}, std::string_view{"GCIE"},
	std::string_view{"GCS"}, std::string_view{"HBC"},
	std::string_view{"HINTE"}, std::string_view{"I8MM"},
	std::string_view{"ITE"}, std::string_view{"JSCVT"},
	std::string_view{"LOR"}, std::string_view{"LRCPC"},
	std::string_view{"LRCPC2"}, std::string_view{"LRCPC3"},
	std::string_view{"LS64"}, std::string_view{"LS64_ACCDATA"},
	std::string_view{"LS64_V"}, std::string_view{"LSCP"},
	std::string_view{"LSE"}, std::string_view{"LSE128"},
	std::string_view{"LSFE"}, std::string_view{"LSUI"},
	std::string_view{"LUT"}, std::string_view{"MEC"},
	std::string_view{"MOPS"}, std::string_view{"MTE"},
	std::string_view{"MTE2"}, std::string_view{"MTETC"},
	std::string_view{"NMI"}, std::string_view{"OCCMO"},
	std::string_view{"PAN"}, std::string_view{"PAN2"},
	std::string_view{"PAuth"}, std::string_view{"PAuth_LR"},
	std::string_view{"PCDPHINT"}, std::string_view{"PMULL"},
	std::string_view{"PRFMSLC"}, std::string_view{"PoPS"},
	std::string_view{"RAS"}, std::string_view{"RDM"},
	std::string_view{"RME"}, std::string_view{"RME_GPC3"},
	std::string_view{"RPRFM"}, std::string_view{"SB"},
	std::string_view{"SHA1"}, std::string_view{"SHA256"},
	std::string_view{"SHA3"}, std::string_view{"SHA512"},
	std::string_view{"SM3"}, std::string_view{"SM4"},
	std::string_view{"SME"}, std::string_view{"SPE"},
	std::string_view{"SPECRES"}, std::string_view{"SPECRES2"},
	std::string_view{"SPE_EXC"}, std::string_view{"SSBS"},
	std::string_view{"SYSINSTR128"}, std::string_view{"SYSREG128"},
	std::string_view{"THE"}, std::string_view{"TLBID"},
	std::string_view{"TLBIOS"}, std::string_view{"TLBIRANGE"},
	std::string_view{"TLBIW"}, std::string_view{"TRBE_EXC"},
	std::string_view{"TRF"}, std::string_view{"UAO"},
	std::string_view{"VHE"}, std::string_view{"WFxT"},
	std::string_view{"XS"}, std::string_view{"AFP"},
};

inline constexpr std::size_t arm_inventory_feature_count =
	static_cast<std::size_t>(ArmFeature::inventory_count);
inline constexpr std::size_t arm_feature_count =
	static_cast<std::size_t>(ArmFeature::count);
static_assert(arm_inventory_feature_count == 107);
static_assert(arm_feature_labels.size() == arm_feature_count);

using ArmFeatureSet = std::bitset<arm_feature_count>;

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

	const auto queried = query_host_arm_feature_sysctl(name.data());
	if (queried != HostCapabilityResult::unavailable)
		return queried == HostCapabilityResult::supported;

	const auto probe = detail::instruction_probe_for(feature);
	return probe != nullptr &&
		probe_host_instruction(probe) == HostCapabilityResult::supported;
}

struct HostArmFeatures final {
	ArmFeatureSet supported;
	bool floating_point{};
	bool advanced_simd{};
	bool fp16{};
	bool cssc{};
	bool aes{};
	bool pmull{};
	bool sha1{};
	bool sha2{};
	bool crc32{};
	bool sha3{};
	bool sha512{};
	bool afp{};
	bool jscvt_subnormal_flags_correct{};

	[[nodiscard]] bool has(ArmFeature feature) const noexcept
	{
		return supported.test(static_cast<std::size_t>(feature));
	}

	[[nodiscard]] bool supports(
		const ArmFeatureSet &requirements) const noexcept
	{
		return (supported & requirements) == requirements;
	}
};

[[nodiscard]] inline const HostArmFeatures &host_arm_features() noexcept
{
	static const HostArmFeatures features = [] {
		HostArmFeatures result;
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

} // namespace uml::backend
