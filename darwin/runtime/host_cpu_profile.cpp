// SPDX-License-Identifier: GPL-2.0-or-later
#include "host_cpu_profile.hpp"

#include "platform/host_topology.hpp"

#include <string>

namespace uml::runtime {

namespace {

std::string join_unsigned(const std::vector<unsigned> &values)
{
	std::string result;
	for (std::size_t index = 0; index < values.size(); ++index) {
		if (index != 0U)
			result.push_back(',');
		result += std::to_string(values[index]);
	}
	return result;
}

} // namespace

HostCpuProfile prepare_apple_host_cpu_profile(
	unsigned guest_cpus, std::string_view configured_capacities)
{
	const auto level_capacities =
		::uml::platform::parse_performance_level_capacities(
			configured_capacities);
	if (!level_capacities) {
		return {
			.status = HostCpuProfileStatus::invalid_configuration,
			.summary = "invalid Apple performance-level capacity list",
		};
	}
	const auto topology = ::uml::platform::host_performance_topology();
	if (!topology) {
		return {
			.status = HostCpuProfileStatus::topology_unavailable,
			.summary = "Apple performance topology unavailable; using "
				"uniform guest CPUs",
		};
	}
	const auto plan = ::uml::platform::make_guest_cpu_capacity_plan(
		*topology, guest_cpus, *level_capacities);
	if (!plan) {
		return {
			.status = HostCpuProfileStatus::topology_unavailable,
			.summary = "Apple performance topology cannot cover the guest "
				"profile; using uniform guest CPUs",
		};
	}
	std::vector<unsigned> host_counts;
	host_counts.reserve(topology->levels.size());
	for (const auto &level : topology->levels)
		host_counts.push_back(level.logical_cpus);
	const std::string summary =
		"host performance levels=" + join_unsigned(host_counts) +
		"; guest CPU capacities=" + join_unsigned(plan->capacities);
	if (!plan->asymmetric()) {
		return {
			.status = HostCpuProfileStatus::uniform,
			.summary = summary,
		};
	}
	return {
		.status = HostCpuProfileStatus::asymmetric,
		.kernel_argument =
			::uml::platform::format_guest_cpu_capacities(*plan),
		.summary = summary,
	};
}

} // namespace uml::runtime
