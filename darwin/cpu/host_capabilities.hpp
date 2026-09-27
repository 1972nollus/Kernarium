// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/host_capabilities.hpp"

namespace uml::cpu {

using ::uml::platform::ArmFeature;
using ::uml::platform::ArmFeatureSet;
using ::uml::platform::HostCapabilities;
using ::uml::platform::arm_feature_count;
using ::uml::platform::arm_feature_labels;
using ::uml::platform::arm_feature_tokens;
using ::uml::platform::arm_inventory_feature_count;

/* Whether two hosts get the same generated code, so their executors may share
 * a pool session or a translation. Compared member by member rather than by
 * bytes so padding cannot tell equal profiles apart; a member added to
 * HostCapabilities must be added here. */
[[nodiscard]] inline bool same_host_capabilities(
	const HostCapabilities &first, const HostCapabilities &second) noexcept
{
	return first.supported == second.supported &&
		first.floating_point == second.floating_point &&
		first.advanced_simd == second.advanced_simd &&
		first.fp16 == second.fp16 && first.cssc == second.cssc &&
		first.aes == second.aes && first.pmull == second.pmull &&
		first.sha1 == second.sha1 && first.sha2 == second.sha2 &&
		first.crc32 == second.crc32 && first.sha3 == second.sha3 &&
		first.sha512 == second.sha512 && first.afp == second.afp &&
		first.jscvt_subnormal_flags_correct ==
			second.jscvt_subnormal_flags_correct;
}

/* Direct CPU unit tests intentionally use only the Apple arm64 ABI baseline.
 * Production engines always receive the complete probed profile from runtime.
 */
[[nodiscard]] inline const HostCapabilities &baseline_host_capabilities() noexcept
{
	static const HostCapabilities capabilities = [] {
		HostCapabilities result;
		result.supported.set(
			static_cast<std::size_t>(ArmFeature::floating_point));
		result.supported.set(
			static_cast<std::size_t>(ArmFeature::advanced_simd));
		result.floating_point = true;
		result.advanced_simd = true;
		return result;
	}();
	return capabilities;
}

inline const HostCapabilities *process_host_capabilities = nullptr;
inline thread_local const HostCapabilities *active_host_capabilities = nullptr;

/* Test and embedding process setup may install one immutable default before
 * worker threads start. Per-engine scopes still take precedence.
 */
[[nodiscard]] inline bool install_process_host_capabilities(
	const HostCapabilities &capabilities) noexcept
{
	if (process_host_capabilities != nullptr)
		return process_host_capabilities == &capabilities;
	process_host_capabilities = &capabilities;
	return true;
}

[[nodiscard]] inline const HostCapabilities &host_capabilities() noexcept
{
	if (active_host_capabilities != nullptr)
		return *active_host_capabilities;
	return process_host_capabilities != nullptr ? *process_host_capabilities :
		baseline_host_capabilities();
}

class HostCapabilitiesScope final {
public:
	explicit HostCapabilitiesScope(
		const HostCapabilities &capabilities) noexcept
		: previous_{active_host_capabilities}
	{
		active_host_capabilities = &capabilities;
	}

	~HostCapabilitiesScope()
	{
		active_host_capabilities = previous_;
	}

	HostCapabilitiesScope(const HostCapabilitiesScope &) = delete;
	HostCapabilitiesScope &operator=(const HostCapabilitiesScope &) = delete;

private:
	const HostCapabilities *previous_;
};

} // namespace uml::cpu
