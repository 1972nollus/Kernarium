// SPDX-License-Identifier: GPL-2.0-or-later
/* The foundation every non-streaming SVE lane builds on: the guest-SVE
 * policy and the prober's SVE tokens, the engines' support levels and what
 * they present, the mask form of the predicates and the WHILE* tables, the
 * state CpuState adds and the reset every change of PSTATE.SM applies, and
 * ID_AA64PFR0_EL1.SVE and ID_AA64ZFR0_EL1. arm64-advertised-features pins
 * the words and registers of named hosts; this checks the rules. */
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/gadgets/sme_support.hpp"
#include "cpu/arm64/gadgets/sve_support.hpp"
#include "cpu/arm64/id_registers.hpp"
#include "cpu/arm64/jit/jit_sme_support.hpp"
#include "cpu/arm64/jit/jit_sve_support.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/arm64/sve_profile.hpp"
#include "cpu/engine_factory.hpp"
#include "cpu/host_capabilities.hpp"
#include "platform/apple/host_features.hpp"
#include "require.hpp"
#include "uml_darwin_cpu.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <vector>

namespace {

using ::uml::platform::ArmFeature;
using ::uml::platform::HostCapabilities;
using ::uml::testing::require;
namespace apple = ::uml::platform::apple;
namespace arm64 = ::uml::cpu::arm64;
namespace detail = ::uml::cpu::arm64::detail;

/* AT_HWCAP's SVE bit, and AT_HWCAP2's SVE2, SVEAES, SVEPMULL, SVESHA3,
 * SVEI8MM and SVEBF16 bits (arm64 Linux's uapi hwcap.h). */
constexpr unsigned long sve_hwcap = 1UL << 22;
constexpr unsigned long sve2_hwcap2 = 1UL << 1;
constexpr unsigned long sveaes_hwcap2 = 1UL << 2;
constexpr unsigned long svepmull_hwcap2 = 1UL << 3;
constexpr unsigned long svesha3_hwcap2 = 1UL << 5;
constexpr unsigned long svei8mm_hwcap2 = 1UL << 9;
constexpr unsigned long svebf16_hwcap2 = 1UL << 12;
constexpr unsigned long sve_hwcap2 = sve2_hwcap2 | sveaes_hwcap2 |
	svepmull_hwcap2 | svesha3_hwcap2 | svei8mm_hwcap2 | svebf16_hwcap2;

[[nodiscard]] HostCapabilities with(std::initializer_list<ArmFeature> bits)
{
	HostCapabilities host;
	for (const auto bit : bits)
		host.supported.set(static_cast<std::size_t>(bit));
	return host;
}

/* Every host feature a part of the profile needs. */
[[nodiscard]] HostCapabilities sve_predicates()
{
	return with({ArmFeature::fp16, ArmFeature::dotprod, ArmFeature::aes,
		ArmFeature::pmull, ArmFeature::sha3, ArmFeature::i8mm,
		ArmFeature::bf16});
}

[[nodiscard]] bool reports_any_sve(const HostCapabilities &host)
{
	for (const auto token : arm64::sve_features)
		if (host.has(token))
			return true;
	return false;
}

[[nodiscard]] std::uint64_t read_id(const HostCapabilities &host,
	std::uint32_t id_register)
{
	const ::uml::cpu::HostCapabilitiesScope scope{host};
	require(detail::emulated_id_register(id_register));
	return detail::emulated_id_register_value(id_register);
}

void policy_parses_only_one()
{
	using apple::GuestSvePolicy;
	for (const char *value : {"", "0", "1 ", " 1", "yes", "11", "true"})
		require(apple::parse_guest_sve_policy(value) ==
			GuestSvePolicy::off, value);
	require(apple::parse_guest_sve_policy(nullptr) == GuestSvePolicy::off);
	require(apple::parse_guest_sve_policy("1") == GuestSvePolicy::on);
}

/* The tokens follow the policy and, part by part, the host features the
 * lowering of that part uses; the rest of the profile never changes. */
void policy_needs_each_predicate()
{
	using apple::GuestSvePolicy;
	const auto all = sve_predicates();
	const auto off = apple::apply_guest_sve_policy(all, GuestSvePolicy::off);
	require(off == all, "the policy off changed a host without SVE");

	/* A host whose own sysctls claimed SVE loses it with the policy off,
	 * and gets the profile's parts, not its own, with it on. */
	auto claimed = all;
	for (const auto token : arm64::sve_features)
		claimed.supported.set(static_cast<std::size_t>(token));
	require(apple::apply_guest_sve_policy(claimed, GuestSvePolicy::off) ==
		all, "a claimed token survived the policy off");
	claimed.supported.reset(static_cast<std::size_t>(ArmFeature::sha3));
	require(!apple::apply_guest_sve_policy(claimed, GuestSvePolicy::on)
		.has(ArmFeature::sve_sha3), "a claimed token survived");

	const auto on = apple::apply_guest_sve_policy(all, GuestSvePolicy::on);
	for (const auto token : arm64::sve_features)
		require(on.has(token));
	require(arm64::implements_sve(on));
	auto masked = on;
	for (const auto token : arm64::sve_features)
		masked.supported.reset(static_cast<std::size_t>(token));
	require(masked == all, "the policy changed a non-SVE capability");

	struct Case final {
		ArmFeature missing;
		std::initializer_list<ArmFeature> lost;
	};
	const Case cases[]{
		{ArmFeature::fp16, {ArmFeature::sve, ArmFeature::sve2,
			ArmFeature::sve_aes, ArmFeature::sve_pmull128,
			ArmFeature::sve_sha3}},
		{ArmFeature::dotprod, {ArmFeature::sve, ArmFeature::sve2,
			ArmFeature::sve_aes, ArmFeature::sve_pmull128,
			ArmFeature::sve_sha3}},
		{ArmFeature::aes, {ArmFeature::sve_aes,
			ArmFeature::sve_pmull128}},
		{ArmFeature::pmull, {ArmFeature::sve_aes,
			ArmFeature::sve_pmull128}},
		{ArmFeature::sha3, {ArmFeature::sve_sha3}},
		/* The SVE I8MM and BF16 forms have no token of their own. */
		{ArmFeature::i8mm, {}},
		{ArmFeature::bf16, {}},
	};
	for (const auto &each : cases) {
		auto host = all;
		host.supported.reset(static_cast<std::size_t>(each.missing));
		const auto result = apple::apply_guest_sve_policy(host,
			GuestSvePolicy::on);
		for (const auto token : arm64::sve_features)
			require(result.has(token) !=
				(std::find(each.lost.begin(), each.lost.end(),
					token) != each.lost.end()),
				"a token does not follow its host feature");
	}
	require(!reports_any_sve(apple::apply_guest_sve_policy(
		HostCapabilities{}, GuestSvePolicy::on)));
}

/* The prober: no SVE token with the policy off; with it on, the derivation
 * above applied to everything else it reports, which on every supported
 * host includes SVE and SVE2. */
void prober_follows_policy()
{
	const auto off = apple::probe_host_capabilities(
		apple::GuestSmePolicy::off, apple::GuestSvePolicy::off);
	const auto on = apple::probe_host_capabilities(
		apple::GuestSmePolicy::off, apple::GuestSvePolicy::on);
	require(!reports_any_sve(off), "policy off reported an SVE token");
	require(off == apple::probe_host_capabilities(
		apple::GuestSmePolicy::off), "the SVE policy defaults on");
	require(on == apple::apply_guest_sve_policy(off,
		apple::GuestSvePolicy::on));
	require(arm64::implements_sve(on) ==
		(off.has(ArmFeature::fp16) && off.has(ArmFeature::dotprod)));
}

/* The process profile, which test binaries install as probed, honours
 * DARWIN_UML_GUEST_SVE. */
void process_profile_honours_environment()
{
	const auto policy = apple::parse_guest_sve_policy(
		std::getenv(apple::guest_sve_policy_variable));
	require(apple::guest_sve_policy() == policy);
	require(apple::probed_host_capabilities() ==
		apple::probe_host_capabilities(apple::guest_sme_policy(), policy));
	std::printf("guest-SVE policy %s, profile %s\n",
		policy == apple::GuestSvePolicy::on ? "on" : "off",
		arm64::implements_sve(apple::probed_host_capabilities()) ?
			"reported" : "not reported");
}

/* Below complete an engine presents no SVE token and changes nothing else;
 * at complete it presents the host as given. Both engines are at complete,
 * so under the policy both present SVE, and so do the entry points the
 * guest kernel reads outside a run (the policy is off unless the
 * environment turns it on). */
void engines_present_sve_only_when_complete()
{
	using arm64::SveSupport;
	const auto host = apple::apply_guest_sve_policy(sve_predicates(),
		apple::GuestSvePolicy::on);
	const auto without = sve_predicates();
	for (const auto support : {SveSupport::none, SveSupport::control})
		require(arm64::sve_host_for_support(host, support) == without);
	require(arm64::sve_host_for_support(host, SveSupport::complete) == host);
	for (const auto support : {SveSupport::none, SveSupport::control,
		     SveSupport::complete})
		require(arm64::sve_host_for_support(without, support) == without);

	require(arm64::gadgets::sve_support == SveSupport::complete &&
		arm64::jit::sve_support == SveSupport::complete);
	for (const auto kind : {::uml::cpu::EngineKind::gadgets,
		     ::uml::cpu::EngineKind::jit}) {
		const auto presented = ::uml::cpu::engine_host_capabilities(kind,
			host);
		require(presented == arm64::sme_host_for_support(host,
			kind == ::uml::cpu::EngineKind::jit ?
				arm64::jit::sme_support :
				arm64::gadgets::sme_support));
		require(arm64::implements_sve(presented));
		const auto absent = ::uml::cpu::engine_host_capabilities(kind,
			without);
		require(!reports_any_sve(absent));
	}
	/* Outside a run the guest kernel reads what the probed host reports
	 * under the policy, and both engines implement all of it. */
	const bool reported =
		arm64::implements_sve(apple::probed_host_capabilities());
	require(((darwin_uml_guest_hwcap() & sve_hwcap) != 0) == reported,
		"the guest kernel's SVE word does not follow the policy");
	require(((darwin_uml_guest_hwcap2() & sve_hwcap2) != 0) == reported,
		"the guest kernel's SVE2 word does not follow the policy");
}

/* The mask form maps one to one onto the 16 predicate bits. */
void predicate_masks_round_trip()
{
	for (unsigned packed = 0; packed <= 0xffffU; ++packed) {
		const auto mask = arm64::sve_predicate_mask(
			static_cast<std::uint16_t>(packed));
		for (unsigned byte = 0; byte < arm64::sve_vector_length; ++byte)
			require(mask[byte] == ((packed >> byte & 1U) != 0U ?
				0xffU : 0x00U));
		require(arm64::sve_predicate_packed(mask) == packed);
	}
}

/* The element patterns, and the WHILE* tables against PredTest() written
 * out bit by bit, for every element size and active count. */
void leading_predicates_follow_predtest()
{
	const auto check = [](unsigned esize, const auto &table) {
		const unsigned elements = arm64::sve_vector_length / esize;
		require(table.size() == elements + 1U);
		const auto pattern = arm64::sve_element_pattern(esize);
		for (unsigned byte = 0; byte < arm64::sve_vector_length; ++byte)
			require(pattern[byte] == (byte % esize == 0 ? 0xffU : 0U));
		for (unsigned active = 0; active <= elements; ++active) {
			const auto &entry = table[active];
			unsigned packed = 0;
			for (unsigned element = 0; element < active; ++element)
				packed |= 1U << (element * esize);
			require(arm64::sve_predicate_packed(entry.predicate) ==
				packed);
			const bool first = (packed & 1U) != 0U;
			const bool none = packed == 0U;
			const bool last = (packed >> ((elements - 1U) * esize) &
				1U) != 0U;
			const std::uint32_t nzcv =
				(first ? 0x8000'0000U : 0U) |
				(none ? 0x4000'0000U : 0U) |
				(last ? 0U : 0x2000'0000U);
			require(entry.nzcv == nzcv);
			require(arm64::sve_leading_predicate(esize, active).nzcv ==
				nzcv);
		}
		require(table[elements].predicate == pattern,
			"the full table entry is not PTRUE");
	};
	check(1, arm64::sve_leading_predicate_table<1>());
	check(2, arm64::sve_leading_predicate_table<2>());
	check(4, arm64::sve_leading_predicate_table<4>());
	check(8, arm64::sve_leading_predicate_table<8>());
}

/* Every change of PSTATE.SM zeroes the non-streaming P and FFR, in both
 * directions; a write that keeps SM, ZA alone included, leaves them. */
void streaming_mode_changes_reset_predicates()
{
	constexpr unsigned svl = 64;
	arm64::SmeRegisterFile file{svl};
	std::vector<std::byte> task(arm64::sme_storage_size(svl));
	const auto filled = [](arm64::CpuState &cpu) {
		for (auto &predicate : cpu.sve_p)
			predicate = arm64::sve_predicate_mask(0xa5a5U);
		cpu.sve_ffr = arm64::sve_predicate_mask(0x00ffU);
	};
	const auto zeroed = [](const arm64::CpuState &cpu) {
		for (const auto &predicate : cpu.sve_p)
			if (predicate != arm64::SvePredicate{})
				return false;
		return cpu.sve_ffr == arm64::SvePredicate{};
	};
	struct Write final {
		std::uint8_t from, mask, value;
		bool resets;
	};
	for (const auto &write : {
		     Write{0, arm64::svcr_sm, arm64::svcr_sm, true},
		     Write{0, arm64::svcr_mask, arm64::svcr_mask, true},
		     Write{arm64::svcr_sm, arm64::svcr_sm, 0, true},
		     Write{arm64::svcr_mask, arm64::svcr_mask, 0, true},
		     Write{0, arm64::svcr_za, arm64::svcr_za, false},
		     Write{arm64::svcr_za, arm64::svcr_za, 0, false},
		     Write{arm64::svcr_sm, arm64::svcr_sm, arm64::svcr_sm, false},
		     Write{0, arm64::svcr_sm, 0, false}}) {
		require(arm64::svcr_write(write.from, write.mask, write.value,
				true).zero_sve_predicates == write.resets);
		arm64::CpuState cpu{};
		cpu.bind_sme_registers(file);
		cpu.defer_sme_load(task.data(), write.from, 0);
		filled(cpu);
		require(cpu.write_svcr(write.mask, write.value));
		require(zeroed(cpu) == write.resets,
			"an SVCR write handled P and FFR wrongly");
	}
	/* A write refused for want of storage changes nothing. */
	arm64::CpuState cpu{};
	cpu.bind_sme_registers(file);
	cpu.defer_sme_load(nullptr, 0, 0);
	filled(cpu);
	require(!cpu.write_svcr(arm64::svcr_sm, arm64::svcr_sm));
	require(!zeroed(cpu));
}

/* PFR0.SVE, ZFR0 and the SVE HWCAP bits, field by field, for hosts with the
 * profile in every combination of its optional parts, and ZFR0 as zero
 * without it, which is what every engine presents today. */
void identification_follows_profile()
{
	require(read_id(HostCapabilities{}, detail::id_aa64zfr0_el1) == 0);
	require(read_id(sve_predicates(), detail::id_aa64zfr0_el1) == 0);
	require((read_id(sve_predicates(), detail::id_aa64pfr0_el1) >> 32 &
		15U) == 0);

	for (unsigned parts = 0; parts < 16U; ++parts) {
		const bool aes = (parts & 1U) != 0U;
		const bool sha3 = (parts & 2U) != 0U;
		const bool i8mm = (parts & 4U) != 0U;
		const bool bf16 = (parts & 8U) != 0U;
		auto host = with({ArmFeature::fp16, ArmFeature::dotprod});
		host.fp16 = true;
		for (const auto &[feature, on] : {std::pair{ArmFeature::aes, aes},
			     std::pair{ArmFeature::pmull, aes},
			     std::pair{ArmFeature::sha3, sha3},
			     std::pair{ArmFeature::i8mm, i8mm},
			     std::pair{ArmFeature::bf16, bf16}})
			host.supported.set(static_cast<std::size_t>(feature), on);
		host.aes = host.pmull = aes;
		host.sha3 = sha3;
		const auto sve = apple::apply_guest_sve_policy(host,
			apple::GuestSvePolicy::on);
		require((read_id(sve, detail::id_aa64pfr0_el1) >> 32 & 15U) == 1);
		const std::uint64_t zfr0 = 1U | (aes ? 2U << 4 : 0U) |
			(bf16 ? std::uint64_t{1} << 20 : 0U) |
			(sha3 ? std::uint64_t{1} << 32 : 0U) |
			(i8mm ? std::uint64_t{1} << 44 : 0U);
		require(read_id(sve, detail::id_aa64zfr0_el1) == zfr0);

		const auto with_sve = detail::guest_hwcaps(sve);
		const auto without = detail::guest_hwcaps(host);
		require(with_sve[0] == (without[0] | sve_hwcap));
		require(with_sve[1] == (without[1] | sve2_hwcap2 |
			(aes ? sveaes_hwcap2 | svepmull_hwcap2 : 0UL) |
			(sha3 ? svesha3_hwcap2 : 0UL) |
			(i8mm ? svei8mm_hwcap2 : 0UL) |
			(bf16 ? svebf16_hwcap2 : 0UL)));
		require((without[0] & sve_hwcap) == 0 &&
			(without[1] & sve_hwcap2) == 0);
	}
}

} // namespace

int main()
{
	policy_parses_only_one();
	policy_needs_each_predicate();
	prober_follows_policy();
	process_profile_honours_environment();
	engines_present_sve_only_when_complete();
	predicate_masks_round_trip();
	leading_predicates_follow_predtest();
	streaming_mode_changes_reset_predicates();
	identification_follows_profile();
	return 0;
}
