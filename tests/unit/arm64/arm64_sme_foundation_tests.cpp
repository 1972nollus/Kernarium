// SPDX-License-Identifier: GPL-2.0-or-later
/* The SME foundation every SME lane builds on: the guest-SME policy and the
 * host prober. */
#include "cpu/host_capabilities.hpp"
#include "platform/apple/host_features.hpp"
#include "require.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/sysctl.h>

namespace {

using ::uml::platform::ArmFeature;
using ::uml::platform::HostCapabilities;
using ::uml::testing::require;
namespace apple = ::uml::platform::apple;

constexpr std::array sme_tokens{ArmFeature::sme, ArmFeature::sme2,
	ArmFeature::sme_f64f64, ArmFeature::sme_i16i64};

[[nodiscard]] bool sysctl_flag(const char *name)
{
	int value = 0;
	std::size_t size = sizeof(value);
	return sysctlbyname(name, &value, &size, nullptr, 0) == 0 &&
		size == sizeof(value) && value != 0;
}

[[nodiscard]] unsigned sysctl_svl()
{
	int value = 0;
	std::size_t size = sizeof(value);
	return sysctlbyname("hw.optional.arm.sme_max_svl_b", &value, &size,
		       nullptr, 0) == 0 && size == sizeof(value) && value > 0 ?
		static_cast<unsigned>(value) : 0U;
}

[[nodiscard]] bool reports_any_sme(const HostCapabilities &host)
{
	for (const auto token : sme_tokens)
		if (host.has(token))
			return true;
	return host.sme_svl_bytes != 0;
}

void policy_parses_only_one()
{
	using apple::GuestSmePolicy;
	require(apple::parse_guest_sme_policy(nullptr) == GuestSmePolicy::off);
	require(apple::parse_guest_sme_policy("") == GuestSmePolicy::off);
	require(apple::parse_guest_sme_policy("0") == GuestSmePolicy::off);
	require(apple::parse_guest_sme_policy("1 ") == GuestSmePolicy::off);
	require(apple::parse_guest_sme_policy("yes") == GuestSmePolicy::off);
	require(apple::parse_guest_sme_policy("11") == GuestSmePolicy::off);
	require(apple::parse_guest_sme_policy("1") == GuestSmePolicy::on);
}

/* With the policy off no host reports an SME token or a vector length, SME
 * hardware or not; sme_hardware still reports FEAT_SME. With it on, the
 * tokens follow the host's sysctls. The rest of the profile never depends
 * on the policy. */
void prober_follows_policy()
{
	const bool hardware = sysctl_flag("hw.optional.arm.FEAT_SME");
	const auto off = apple::probe_host_capabilities(
		apple::GuestSmePolicy::off);
	const auto on = apple::probe_host_capabilities(
		apple::GuestSmePolicy::on);
	require(!reports_any_sme(off), "policy off reported an SME token");
	require(off.sme_hardware == hardware && on.sme_hardware == hardware,
		"sme_hardware does not follow FEAT_SME");

	const unsigned svl = sysctl_svl();
	const bool valid_svl = svl >= 16 && svl <= 256 && (svl & (svl - 1)) == 0;
	if (hardware && valid_svl) {
		require(on.sme_svl_bytes == svl);
		require(on.has(ArmFeature::sme));
		require(on.has(ArmFeature::sme2) ==
			sysctl_flag("hw.optional.arm.FEAT_SME2"));
		require(on.has(ArmFeature::sme_f64f64) ==
			sysctl_flag("hw.optional.arm.FEAT_SME_F64F64"));
		require(on.has(ArmFeature::sme_i16i64) ==
			sysctl_flag("hw.optional.arm.FEAT_SME_I16I64"));
	} else {
		require(!reports_any_sme(on),
			"a host without SME reported an SME token");
	}

	auto masked = on;
	for (const auto token : sme_tokens)
		masked.supported.reset(static_cast<std::size_t>(token));
	masked.sme_svl_bytes = 0;
	require(masked == off, "the policy changed a non-SME capability");
}

/* The process profile, which test binaries install as probed, honours
 * DARWIN_UML_GUEST_SME. */
void process_profile_honours_environment()
{
	const auto policy = apple::parse_guest_sme_policy(
		std::getenv(apple::guest_sme_policy_variable));
	require(apple::guest_sme_policy() == policy);
	require(apple::probed_host_capabilities() ==
		apple::probe_host_capabilities(policy));
	require(::uml::cpu::host_capabilities() ==
		apple::probed_host_capabilities());
	std::printf("guest-SME policy %s, host SME %s, SVL %u\n",
		policy == apple::GuestSmePolicy::on ? "on" : "off",
		apple::probed_host_capabilities().sme_hardware ? "yes" : "no",
		static_cast<unsigned>(
			apple::probed_host_capabilities().sme_svl_bytes));
}

} // namespace

int main()
{
	policy_parses_only_one();
	prober_follows_policy();
	process_profile_honours_environment();
	return 0;
}
