// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace uml::runtime {

inline constexpr std::string_view apple_cpu_capacities_option{
	"--apple-cpu-capacities="};
inline constexpr std::string_view darwin_cpu_capacity_argument{
	"darwin_cpu_capacity_v1="};

enum class HostCpuProfileStatus : std::uint8_t {
	invalid_configuration,
	topology_unavailable,
	uniform,
	asymmetric,
};

struct HostCpuProfile final {
	HostCpuProfileStatus status{HostCpuProfileStatus::topology_unavailable};
	std::string kernel_argument;
	std::string summary;
};

[[nodiscard]] HostCpuProfile prepare_apple_host_cpu_profile(
	unsigned guest_cpus, std::string_view configured_capacities);

} // namespace uml::runtime
