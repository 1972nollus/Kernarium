// SPDX-License-Identifier: GPL-2.0-or-later
/* The foundation every AArch32 lane builds on: the state CpuState appends
 * and its assembly offsets, the guest-AArch32 policy, the engines' support
 * levels and what a run presents of them, the AArch32 identification
 * registers and compat HWCAPs derived from the profile, and the AArch32
 * entry of an engine below base, which is the illegal-instruction exit
 * until a lane raises the level. The keys are in arm64-a32-guest-key. */
#include "cpu/arm64/aarch32/a32_profile.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/gadgets/a32_support.hpp"
#include "cpu/arm64/id_registers.hpp"
#include "cpu/arm64/jit/jit_a32_support.hpp"
#include "cpu/arm64/layout/assembly_offsets.h"
#include "cpu/engine.hpp"
#include "cpu/engine_factory.hpp"
#include "cpu/host_capabilities.hpp"
#include "memory/static_guest_memory.hpp"
#include "platform/apple/host_features.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <type_traits>

namespace {

using ::uml::platform::ArmFeature;
using ::uml::platform::HostCapabilities;
using ::uml::testing::require;
namespace apple = ::uml::platform::apple;
namespace arm64 = ::uml::cpu::arm64;
namespace detail = ::uml::cpu::arm64::detail;

using arm64::A32Support;
using detail::A32IdRegister;

constexpr std::array all_levels{A32Support::none, A32Support::base,
	A32Support::simd, A32Support::complete};

/* The host features an AArch32 part lowers onto, as flags a test can
 * enumerate. */
struct Predicates final {
	bool aes, pmull, sha1, sha2, crc32, fp16, dotprod, fhm, bf16, i8mm,
		sb, rdm, fcma, jscvt;
};
constexpr unsigned predicate_count = 14;

[[nodiscard]] Predicates predicates_of(unsigned bits) noexcept
{
	const auto bit = [bits](unsigned index) { return (bits >> index & 1U) != 0U; };
	return {bit(0), bit(1), bit(2), bit(3), bit(4), bit(5), bit(6),
		bit(7), bit(8), bit(9), bit(10), bit(11), bit(12), bit(13)};
}

[[nodiscard]] HostCapabilities host_with(const Predicates &each)
{
	HostCapabilities host;
	const auto set = [&host](ArmFeature feature, bool on) {
		if (on)
			host.supported.set(static_cast<std::size_t>(feature));
	};
	host.floating_point = true;
	host.advanced_simd = true;
	set(ArmFeature::floating_point, true);
	set(ArmFeature::advanced_simd, true);
	host.aes = each.aes;
	set(ArmFeature::aes, each.aes);
	host.pmull = each.pmull;
	set(ArmFeature::pmull, each.pmull);
	host.sha1 = each.sha1;
	host.sha2 = each.sha2;
	host.crc32 = each.crc32;
	set(ArmFeature::crc32, each.crc32);
	host.fp16 = each.fp16;
	set(ArmFeature::fp16, each.fp16);
	set(ArmFeature::dotprod, each.dotprod);
	set(ArmFeature::fhm, each.fhm);
	set(ArmFeature::bf16, each.bf16);
	set(ArmFeature::i8mm, each.i8mm);
	set(ArmFeature::sb, each.sb);
	set(ArmFeature::rdm, each.rdm);
	set(ArmFeature::fcma, each.fcma);
	set(ArmFeature::jscvt, each.jscvt);
	return host;
}

[[nodiscard]] HostCapabilities full_host()
{
	return host_with(predicates_of((1U << predicate_count) - 1U));
}

[[nodiscard]] std::uint32_t id(const HostCapabilities &host,
	A32Support support, A32IdRegister which)
{
	return detail::a32_id_register_value(host, support, which);
}

constexpr std::array all_id_registers{A32IdRegister::midr,
	A32IdRegister::pfr0, A32IdRegister::pfr1, A32IdRegister::pfr2,
	A32IdRegister::isar0, A32IdRegister::isar1, A32IdRegister::isar2,
	A32IdRegister::isar3, A32IdRegister::isar4, A32IdRegister::isar5,
	A32IdRegister::isar6, A32IdRegister::mvfr0, A32IdRegister::mvfr1,
	A32IdRegister::mvfr2};

/* The state appended to CpuState sits after the reserved block, so no earlier
 * offset moves, at the offsets the assembly and the emitted code use. */
void state_is_appended_at_the_pinned_offsets()
{
	static_assert(std::is_trivially_copyable_v<arm64::CpuState>);
	require(offsetof(arm64::CpuState, a32_ge_mask) == 1508 &&
		UML_ARM64_CPU_A32_GE_MASK == 1508);
	require(offsetof(arm64::CpuState, a32_fpscr_nzcv) == 1512 &&
		UML_ARM64_CPU_A32_FPSCR_NZCV == 1512);
	require(offsetof(arm64::CpuState, a32_tpidruro) == 1516 &&
		UML_ARM64_CPU_A32_TPIDRURO == 1516);
	require(offsetof(arm64::CpuState, a32_q) == 1520 &&
		UML_ARM64_CPU_A32_Q == 1520);
	require(offsetof(arm64::CpuState, a32_itstate) == 1521 &&
		UML_ARM64_CPU_A32_ITSTATE == 1521);
	require(offsetof(arm64::CpuState, a32_cp15ben) == 1522 &&
		UML_ARM64_CPU_A32_CP15BEN == 1522);
	require(offsetof(arm64::CpuState, a32) == 1523 &&
		UML_ARM64_CPU_A32 == 1523);
	/* The context-synchronization pair follows the AArch32 block. */
	require(offsetof(arm64::CpuState, code_sequence) == 1528 &&
		UML_ARM64_CPU_CODE_SEQUENCE == 1528);
	require(offsetof(arm64::CpuState, code_sequence_seen) == 1536 &&
		UML_ARM64_CPU_CODE_SEQUENCE_SEEN == 1536);
	/* The JIT's staging for a page-crossing Advanced SIMD load follows it. */
	require(offsetof(arm64::CpuState, a32_cross) == 1552 &&
		sizeof(arm64::CpuState) == 1616 &&
		alignof(arm64::CpuState) == 16);

	/* A fresh state is an A64 one: every AArch32 field is zero. */
	const arm64::CpuState cpu{};
	require(cpu.a32 == 0 && cpu.a32_q == 0 && cpu.a32_itstate == 0 &&
		cpu.a32_cp15ben == 0 && cpu.a32_ge_mask == 0 &&
		cpu.a32_fpscr_nzcv == 0 && cpu.a32_tpidruro == 0);
}

void policy_parses_only_one()
{
	using apple::GuestAarch32Policy;
	for (const char *value : {"", "0", "1 ", " 1", "yes", "11", "true", "on"})
		require(apple::parse_guest_aarch32_policy(value) ==
			GuestAarch32Policy::off, value);
	require(apple::parse_guest_aarch32_policy(nullptr) ==
		GuestAarch32Policy::off);
	require(apple::parse_guest_aarch32_policy("1") ==
		GuestAarch32Policy::on);

	/* The process policy is the environment's, read once, and the other
	 * policies do not follow it. */
	const auto policy = apple::parse_guest_aarch32_policy(
		std::getenv(apple::guest_aarch32_policy_variable));
	require(apple::guest_aarch32_policy() == policy);
	std::printf("guest-AArch32 policy %s\n",
		policy == apple::GuestAarch32Policy::on ? "on" : "off");
}

/* The policy off presents nothing, whatever the engine implements; on
 * presents exactly the engine's level. */
void presented_support_follows_policy_and_level()
{
	for (const auto level : all_levels) {
		require(arm64::a32_presented_support(level, false) ==
			A32Support::none);
		require(arm64::a32_presented_support(level, true) == level);
	}
}

/* Under the policy off an engine presents no AArch32 whatever it implements;
 * on, it presents exactly its level; and the factory agrees with the
 * constants. */
void engines_present_their_level_under_the_policy()
{
	require(::uml::cpu::engine_a32_support(::uml::cpu::EngineKind::gadgets) ==
		arm64::gadgets::a32_support);
	require(::uml::cpu::engine_a32_support(::uml::cpu::EngineKind::jit) ==
		arm64::jit::a32_support);
	for (const auto kind : {::uml::cpu::EngineKind::gadgets,
		     ::uml::cpu::EngineKind::jit}) {
		const auto level = ::uml::cpu::engine_a32_support(kind);
		require(arm64::a32_presented_support(level, false) ==
			A32Support::none);
		require(arm64::a32_presented_support(level, true) == level);
	}
}

/* A level presents its parts and nothing above it, and each host-dependent
 * part follows exactly its A64 host predicate. */
void features_follow_level_and_host()
{
	const auto empty = arm64::a32_features(HostCapabilities{},
		A32Support::none);
	require(empty == arm64::A32Features{});
	for (const auto level : all_levels)
		require(arm64::a32_features(HostCapabilities{}, level).aarch32 ==
			(level >= A32Support::base));

	const auto full = full_host();
	require(arm64::a32_features(full, A32Support::none) == empty,
		"a host feature leaked into a level without AArch32");
	auto features = arm64::a32_features(full, A32Support::base);
	require(features.aarch32 && features.acquire_release && !features.vfp &&
		!features.neon && !features.armv8 && !features.crc32 &&
		!features.aes && !features.pmull && !features.sha1 &&
		!features.sha2 && !features.fp16 && !features.dotprod &&
		!features.fhm && !features.bf16 && !features.i8mm &&
		!features.sb && !features.rdm && !features.fcma &&
		!features.jscvt);
	features = arm64::a32_features(full, A32Support::simd);
	require(features.aarch32 && features.acquire_release && features.vfp &&
		features.neon && !features.armv8 && !features.crc32 &&
		!features.aes && !features.fp16 && !features.dotprod &&
		!features.jscvt);
	features = arm64::a32_features(full, A32Support::complete);
	require(features.aarch32 && features.acquire_release && features.vfp &&
		features.neon && features.armv8 && features.crc32 && features.aes &&
		features.pmull && features.sha1 && features.sha2 &&
		features.fp16 && features.dotprod && features.fhm &&
		features.bf16 && features.i8mm && features.sb && features.rdm &&
		features.fcma && features.jscvt);

	for (unsigned bits = 0; bits < 1U << predicate_count; ++bits) {
		const auto each = predicates_of(bits);
		const auto got = arm64::a32_features(host_with(each),
			A32Support::complete);
		require(got.crc32 == each.crc32 && got.aes == each.aes &&
			got.pmull == (each.aes && each.pmull) &&
			got.sha1 == each.sha1 && got.sha2 == each.sha2 &&
			got.fp16 == each.fp16 && got.dotprod == each.dotprod &&
			got.fhm == each.fhm && got.bf16 == each.bf16 &&
			got.i8mm == each.i8mm && got.sb == each.sb &&
			got.rdm == each.rdm && got.fcma == each.fcma &&
			got.jscvt == each.jscvt, "a feature does not follow its host predicate");
	}
}

/* The decoder's profile is exactly what the guest is shown: the level, and
 * of the optional extensions only the ones the host has, so a form of any
 * other extension is UNDEFINED. Below the complete level there are none. */
void decode_profile_is_what_the_guest_is_shown()
{
	namespace dec = arm64::aarch32;
	for (const auto level : all_levels) {
		const auto profile = arm64::a32_decode_profile(full_host(), level);
		require(profile.level == static_cast<dec::Level>(level));
		require((profile.features == dec::FeatureSet::all()) ==
			(level == A32Support::complete),
			"a level below complete decoded an extension");
		if (level != A32Support::complete)
			require(profile.features == dec::Profile::at(profile.level).features);
	}
	for (unsigned bits = 0; bits < 1U << predicate_count; ++bits) {
		const auto each = predicates_of(bits);
		const auto got = arm64::a32_decode_profile(host_with(each),
			A32Support::complete).features;
		require(got.has(dec::Feature::crc32) == each.crc32 &&
			got.has(dec::Feature::aes) == each.aes &&
			got.has(dec::Feature::pmull) == (each.aes && each.pmull) &&
			got.has(dec::Feature::sha1) == each.sha1 &&
			got.has(dec::Feature::sha2) == each.sha2 &&
			got.has(dec::Feature::fp16) == each.fp16 &&
			got.has(dec::Feature::dotprod) == each.dotprod &&
			got.has(dec::Feature::fhm) == each.fhm &&
			got.has(dec::Feature::bf16) == each.bf16 &&
			got.has(dec::Feature::i8mm) == each.i8mm &&
			got.has(dec::Feature::sb) == each.sb &&
			got.has(dec::Feature::rdm) == each.rdm &&
			got.has(dec::Feature::fcma) == each.fcma &&
			got.has(dec::Feature::jscvt) == each.jscvt,
			"a decoded extension does not follow its host predicate");
	}
}

/* No AArch32 below base: every register reads zero and both HWCAP words are
 * zero, so production, which presents none, changes nothing. */
void nothing_is_presented_below_base()
{
	for (const auto &host : {HostCapabilities{}, full_host()}) {
		for (const auto which : all_id_registers)
			require(id(host, A32Support::none, which) == 0,
				"a register was nonzero without AArch32");
		require(detail::guest_compat_hwcaps(host, A32Support::none) ==
			std::array<unsigned long, 2>{0, 0});
	}
}

/* The registers of each level, pinned. These are Cortex-A72's AArch32
 * values adjusted to the profile; a change here is a change to what a
 * 32-bit guest can conclude about the CPU. */
void id_registers_are_pinned_per_level()
{
	const auto full = full_host();
	for (const auto level : {A32Support::base, A32Support::simd,
		     A32Support::complete}) {
		require(id(full, level, A32IdRegister::midr) == 0x000f'0000U);
		require(id(full, level, A32IdRegister::pfr0) == 0x131U);
		require(id(full, level, A32IdRegister::pfr1) == 0x1'0000U);
		require(id(full, level, A32IdRegister::pfr2) == 0U);
		require(id(full, level, A32IdRegister::isar0) == 0x0210'1110U);
		require(id(full, level, A32IdRegister::isar1) == 0x1311'2111U);
		require(id(full, level, A32IdRegister::isar3) == 0x0111'2131U);
		require(id(full, level, A32IdRegister::isar4) == 0x0001'1142U);
	}
	/* The acquire/release group is part of the integer core: LoadStore is 2
	 * (LDRD, STRD, LDA and STL) and ISAR5.SEVL is 1 from base on. */
	for (const auto level : {A32Support::base, A32Support::simd,
		     A32Support::complete}) {
		require(id(full, level, A32IdRegister::isar2) == 0x2123'2042U);
		require((id(full, level, A32IdRegister::isar5) & 0xfU) == 1U,
			"SEVL is not presented");
	}

	/* Below simd there is no floating point; VFP and Advanced SIMD arrive
	 * with it, half precision to the conversions, and the Armv8 forms and
	 * extensions only at complete. */
	for (const auto which : {A32IdRegister::mvfr0, A32IdRegister::mvfr1,
		     A32IdRegister::mvfr2, A32IdRegister::isar6})
		require(id(full, A32Support::base, which) == 0);
	require(id(full, A32Support::base, A32IdRegister::isar5) == 1U);
	require(id(full, A32Support::simd, A32IdRegister::mvfr0) == 0x1011'0222U);
	require(id(full, A32Support::simd, A32IdRegister::mvfr1) == 0x1111'1111U);
	require(id(full, A32Support::simd, A32IdRegister::mvfr2) == 0U);
	require(id(full, A32Support::simd, A32IdRegister::isar5) == 1U);
	require(id(full, A32Support::simd, A32IdRegister::isar6) == 0U);
	require(id(full, A32Support::complete, A32IdRegister::mvfr0) ==
		0x1011'0222U);
	require(id(full, A32Support::complete, A32IdRegister::mvfr1) ==
		0x1321'1111U);
	require(id(full, A32Support::complete, A32IdRegister::mvfr2) == 0x43U);
	require(id(full, A32Support::complete, A32IdRegister::isar5) ==
		0x1101'1121U);
	require(id(full, A32Support::complete, A32IdRegister::isar6) ==
		0x0110'1111U);

	/* A host with nothing optional still presents the complete level's
	 * unconditional parts: the Armv8 floating-point forms and FP16
	 * conversions without arithmetic, besides the acquire/release group
	 * and SEVL. */
	const auto bare = host_with(predicates_of(0));
	require(id(bare, A32Support::complete, A32IdRegister::isar5) == 1U);
	require(id(bare, A32Support::complete, A32IdRegister::isar6) == 0U);
	require(id(bare, A32Support::complete, A32IdRegister::mvfr1) ==
		0x1211'1111U);
	require(id(bare, A32Support::complete, A32IdRegister::mvfr2) == 0x43U);
	require(id(bare, A32Support::complete, A32IdRegister::isar2) ==
		0x2123'2042U);

	/* Swap is 0 (SWP is UNDEFINED) and, in MVFR0, FPTrap and FPShVec are 0
	 * (the trap enables and short vectors are RAZ/WI). Endian stays 1, as
	 * on the reference core: SCTLR.SED makes SETEND UNDEFINED, not absent
	 * from the identification. */
	for (const auto level : all_levels) {
		require((id(full, level, A32IdRegister::isar0) & 0xfU) == 0,
			"the Swap field says SWP exists");
		require((id(full, level, A32IdRegister::mvfr0) >> 12 & 0xfU) == 0,
			"the FPTrap field says trap enables exist");
		require((id(full, level, A32IdRegister::mvfr0) >> 24 & 0xfU) == 0,
			"the FPShVec field says short vectors exist");
	}
}

/* Each optional register field follows its host predicate, subset by
 * subset, and the HWCAP words are the arm64 Linux derivation of the
 * registers written out by hand. */
void optional_parts_follow_the_host()
{
	constexpr unsigned long fphp = 1UL << 22;
	constexpr unsigned long asimdhp = 1UL << 23;
	constexpr unsigned long asimddp = 1UL << 24;
	constexpr unsigned long asimdfhm = 1UL << 25;
	constexpr unsigned long asimdbf16 = 1UL << 26;
	constexpr unsigned long i8mm = 1UL << 27;
	constexpr unsigned long aes2 = 1UL << 0;
	constexpr unsigned long pmull2 = 1UL << 1;
	constexpr unsigned long sha1 = 1UL << 2;
	constexpr unsigned long sha2 = 1UL << 3;
	constexpr unsigned long crc32 = 1UL << 4;
	constexpr unsigned long sb = 1UL << 5;

	for (unsigned bits = 0; bits < 1U << predicate_count; ++bits) {
		const auto each = predicates_of(bits);
		const auto host = host_with(each);
		const bool pmull = each.aes && each.pmull;

		const auto isar5 = id(host, A32Support::complete,
			A32IdRegister::isar5);
		require(isar5 == ((each.fcma ? 1U : 0U) << 28 |
			(each.rdm ? 1U : 0U) << 24 | (each.crc32 ? 1U : 0U) << 16 |
			(each.sha2 ? 1U : 0U) << 12 | (each.sha1 ? 1U : 0U) << 8 |
			(each.aes ? (pmull ? 2U : 1U) : 0U) << 4 | 1U));
		const auto isar6 = id(host, A32Support::complete,
			A32IdRegister::isar6);
		require(isar6 == ((each.i8mm ? 1U : 0U) << 24 |
			(each.bf16 ? 1U : 0U) << 20 | (each.sb ? 1U : 0U) << 12 |
			(each.fhm ? 1U : 0U) << 8 | (each.dotprod ? 1U : 0U) << 4 |
			(each.jscvt ? 1U : 0U)));
		const auto mvfr1 = id(host, A32Support::complete,
			A32IdRegister::mvfr1);
		require((mvfr1 >> 24 & 15U) == (each.fp16 ? 3U : 2U) &&
			(mvfr1 >> 20 & 15U) == (each.fp16 ? 2U : 1U));

		unsigned long word0 = 0x0037'b0d6UL;
		word0 |= each.fp16 ? fphp | asimdhp : 0UL;
		word0 |= each.dotprod ? asimddp : 0UL;
		word0 |= each.fhm ? asimdfhm : 0UL;
		word0 |= each.bf16 ? asimdbf16 : 0UL;
		word0 |= each.i8mm ? i8mm : 0UL;
		unsigned long word1 = 0;
		word1 |= each.aes ? aes2 : 0UL;
		word1 |= pmull ? pmull2 : 0UL;
		word1 |= each.sha1 ? sha1 : 0UL;
		word1 |= each.sha2 ? sha2 : 0UL;
		word1 |= each.crc32 ? crc32 : 0UL;
		word1 |= each.sb ? sb : 0UL;
		require(detail::guest_compat_hwcaps(host, A32Support::complete) ==
			std::array<unsigned long, 2>{word0, word1},
			"the compat HWCAPs do not follow the registers");
		/* The host's optional features never reach a lower level. */
		require(detail::guest_compat_hwcaps(host, A32Support::simd) ==
			std::array<unsigned long, 2>{0x0037'b0d6UL, 0});
		require(detail::guest_compat_hwcaps(host, A32Support::base) ==
			std::array<unsigned long, 2>{0x0036'8096UL, 0});
	}
}

/* The words of each level on every host. */
void compat_hwcaps_are_pinned_per_level()
{
	constexpr unsigned long default_word = 0x0036'8096UL;
	constexpr unsigned long vfp = 1UL << 6;
	constexpr unsigned long neon = 1UL << 12;
	constexpr unsigned long vfpv3 = 1UL << 13;
	constexpr unsigned long vfpv4 = 1UL << 16;
	require(detail::a32_default_hwcap == default_word);
	require(detail::a32_hwcap_neon == neon);
	for (const auto &host : {HostCapabilities{}, full_host()}) {
		require(detail::guest_compat_hwcaps(host, A32Support::base) ==
			std::array<unsigned long, 2>{default_word, 0});
		require(detail::guest_compat_hwcaps(host, A32Support::simd) ==
			std::array<unsigned long, 2>{
				default_word | vfp | neon | vfpv3 | vfpv4, 0});
	}
	/* The default word's contents: HALF, THUMB, FAST_MULT, EDSP, TLS, IDIVA,
	 * IDIVT, LPAE and EVTSTRM; never SWP, VFPD32 or the 26-bit and
	 * FPA words of old ABIs. */
	for (const unsigned absent : {0U, 3U, 5U, 8U, 9U, 10U, 11U, 14U, 19U})
		require((default_word >> absent & 1UL) == 0, "an absent bit is set");
	for (const unsigned present : {1U, 2U, 4U, 7U, 15U, 17U, 18U, 20U, 21U})
		require((default_word >> present & 1UL) != 0, "a bit is missing");
}

/* The AArch32 entry of an engine below base is the illegal-instruction exit
 * and leaves the state and the guest memory as it found them. An engine at
 * base or above has its own execution tests. */
void aarch32_entry_is_illegal_below_base()
{
	for (const auto kind : {::uml::cpu::EngineKind::gadgets,
		     ::uml::cpu::EngineKind::jit}) {
		if (!::uml::cpu::cpu_engine_available(kind)) {
			std::printf("engine %d unavailable on this host; skipped\n",
				static_cast<int>(kind));
			continue;
		}
		if (::uml::cpu::engine_a32_support(kind) != A32Support::none)
			continue;
		auto engine = ::uml::cpu::make_cpu_engine(kind);
		require(engine != nullptr && engine->kind() == kind);
		if (::uml::cpu::engine_a32_support(kind) != A32Support::none)
			continue;
		for (const unsigned long generation : {1UL, 7UL}) {
			arm64::CpuState cpu{};
			cpu.pc = arm64::aarch32::aarch32_key_bit | 0x1000U;
			cpu.a32 = 1;
			cpu.a32_itstate = 0x0c;
			cpu.a32_q = 1;
			cpu.a32_ge_mask = 0x00ff'ff00U;
			cpu.a32_fpscr_nzcv = 0x6000'0000U;
			cpu.a32_tpidruro = 0xb6f0'0000U;
			cpu.a32_cp15ben = 1;
			for (std::size_t index = 0; index < 15; ++index)
				cpu.registers[index] = 0x1111'0000U + index;
			cpu.nzcv = 0x2000'0000U;
			std::array<std::byte, sizeof(cpu)> before{};
			std::memcpy(before.data(), &cpu, sizeof(cpu));

			::uml::memory::StaticGuestMemory memory;
			memory.set_generation(generation);
			::uml::cpu::ExecutionRequest request{
				.cpu = cpu,
				.memory = memory,
				.translated_memory = &memory,
				.generation = generation,
				.instruction_budget = 8,
			};
			const auto result = engine->run_aarch32(request);
			require(result.reason ==
				::uml::cpu::ExitReason::illegal_instruction,
				"an AArch32 entry did not report illegal");
			require(result.retired_instructions == 0,
				"an AArch32 entry retired an instruction");
			std::array<std::byte, sizeof(cpu)> after{};
			std::memcpy(after.data(), &cpu, sizeof(cpu));
			require(before == after, "an AArch32 entry changed the state");
			require(!memory.has_fault(),
				"an AArch32 entry touched guest memory");
		}
	}
}

} // namespace

int main()
{
	state_is_appended_at_the_pinned_offsets();
	policy_parses_only_one();
	presented_support_follows_policy_and_level();
	engines_present_their_level_under_the_policy();
	features_follow_level_and_host();
	decode_profile_is_what_the_guest_is_shown();
	nothing_is_presented_below_base();
	id_registers_are_pinned_per_level();
	optional_parts_follow_the_host();
	compat_hwcaps_are_pinned_per_level();
	aarch32_entry_is_illegal_below_base();
	return 0;
}
