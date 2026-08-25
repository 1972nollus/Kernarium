// SPDX-License-Identifier: GPL-2.0-or-later
#include "host_topology.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <limits>
#include <utility>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace uml::platform {

unsigned online_processor_count() noexcept
{
	const auto count = sysconf(_SC_NPROCESSORS_ONLN);
	if (count <= 0)
		return 1;
	if (static_cast<unsigned long>(count) >
	    std::numeric_limits<unsigned>::max())
		return std::numeric_limits<unsigned>::max();
	return static_cast<unsigned>(count);
}

namespace {

std::optional<unsigned> apple_integer_sysctl(
	void *, std::string_view name) noexcept
{
#if defined(__APPLE__)
	std::array<char, 64> terminated{};
	if (name.empty() || name.size() >= terminated.size())
		return std::nullopt;
	std::copy(name.begin(), name.end(), terminated.begin());
	int value = 0;
	std::size_t size = sizeof(value);
	if (::sysctlbyname(
			terminated.data(), &value, &size, nullptr, 0) != 0 ||
	    size != sizeof(value) || value < 0)
		return std::nullopt;
	return static_cast<unsigned>(value);
#else
	(void)name;
	return std::nullopt;
#endif
}

bool checked_add(unsigned value, unsigned &total) noexcept
{
	if (value > std::numeric_limits<unsigned>::max() - total)
		return false;
	total += value;
	return true;
}

} // namespace

std::optional<HostPerformanceTopology> discover_host_performance_topology(
	HostTopologyIntegerQuery query, void *context)
{
	if (query == nullptr)
		return std::nullopt;
	const auto level_count = query(context, "hw.nperflevels");
	const auto active_total = query(context, "hw.logicalcpu");
	const auto maximum_total = query(context, "hw.logicalcpu_max");
	if (!level_count || *level_count == 0U ||
	    *level_count > maximum_host_performance_levels ||
	    !active_total || *active_total == 0U ||
	    !maximum_total || *maximum_total == 0U ||
	    *active_total > *maximum_total)
		return std::nullopt;

	HostPerformanceTopology topology;
	topology.levels.reserve(*level_count);
	unsigned observed_active = 0;
	unsigned observed_maximum = 0;
	for (unsigned level = 0; level < *level_count; ++level) {
		std::array<char, 64> active_name{};
		std::array<char, 64> maximum_name{};
		const int active_length = std::snprintf(
			active_name.data(), active_name.size(),
			"hw.perflevel%u.logicalcpu", level);
		const int maximum_length = std::snprintf(
			maximum_name.data(), maximum_name.size(),
			"hw.perflevel%u.logicalcpu_max", level);
		if (active_length <= 0 || maximum_length <= 0 ||
		    static_cast<std::size_t>(active_length) >= active_name.size() ||
		    static_cast<std::size_t>(maximum_length) >= maximum_name.size())
			return std::nullopt;
		const auto active = query(context, active_name.data());
		const auto maximum = query(context, maximum_name.data());
		if (!active || *active == 0U || !maximum || *maximum == 0U ||
		    *active > *maximum ||
		    !checked_add(*active, observed_active) ||
		    !checked_add(*maximum, observed_maximum))
			return std::nullopt;
		topology.levels.push_back({
			.logical_cpus = *active,
			.maximum_logical_cpus = *maximum,
		});
	}
	if (observed_active != *active_total ||
	    observed_maximum != *maximum_total)
		return std::nullopt;
	return topology;
}

std::optional<HostPerformanceTopology> host_performance_topology()
{
	return discover_host_performance_topology(apple_integer_sysctl, nullptr);
}

bool GuestCpuCapacityPlan::asymmetric() const noexcept
{
	return std::ranges::any_of(capacities, [](unsigned capacity) {
		return capacity != normalized_cpu_capacity;
	});
}

std::optional<std::vector<unsigned>> parse_performance_level_capacities(
	std::string_view text)
{
	std::vector<unsigned> capacities;
	while (!text.empty()) {
		const auto separator = text.find(',');
		const auto token = text.substr(0, separator);
		if (token.empty())
			return std::nullopt;
		unsigned value = 0;
		const auto parsed = std::from_chars(
			token.data(), token.data() + token.size(), value, 10);
		if (parsed.ec != std::errc{} ||
		    parsed.ptr != token.data() + token.size() ||
		    value == 0U || value > normalized_cpu_capacity ||
		    capacities.size() >= maximum_host_performance_levels)
			return std::nullopt;
		if (capacities.empty()) {
			if (value != normalized_cpu_capacity)
				return std::nullopt;
		} else if (value >= capacities.back()) {
			return std::nullopt;
		}
		capacities.push_back(value);
		if (separator == std::string_view::npos)
			break;
		text.remove_prefix(separator + 1U);
		if (text.empty())
			return std::nullopt;
	}
	return capacities.empty()
		? std::nullopt
		: std::optional<std::vector<unsigned>>{std::move(capacities)};
}

std::optional<GuestCpuCapacityPlan> make_guest_cpu_capacity_plan(
	const HostPerformanceTopology &topology, unsigned guest_cpus,
	const std::vector<unsigned> &level_capacities)
{
	if (guest_cpus == 0U || topology.levels.empty() ||
	    topology.levels.size() > level_capacities.size())
		return std::nullopt;
	GuestCpuCapacityPlan plan;
	plan.capacities.reserve(guest_cpus);
	plan.performance_levels.reserve(guest_cpus);
	unsigned remaining = guest_cpus;
	for (std::size_t level = 0;
	     level < topology.levels.size() && remaining != 0U; ++level) {
		const auto count = std::min(
			topology.levels[level].logical_cpus, remaining);
		for (unsigned index = 0; index < count; ++index) {
			plan.capacities.push_back(level_capacities[level]);
			plan.performance_levels.push_back(
				static_cast<unsigned>(level));
		}
		remaining -= count;
	}
	if (remaining != 0U)
		return std::nullopt;
	return plan;
}

std::string format_guest_cpu_capacities(const GuestCpuCapacityPlan &plan)
{
	std::string result{"darwin_cpu_capacity_v1="};
	for (std::size_t index = 0; index < plan.capacities.size(); ++index) {
		if (index != 0U)
			result.push_back(',');
		result += std::to_string(plan.capacities[index]);
	}
	return result;
}

} // namespace uml::platform
