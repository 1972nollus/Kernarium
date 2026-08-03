// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <string_view>

namespace uml::platform {

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

struct HostCapabilities final {
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

} // namespace uml::platform
