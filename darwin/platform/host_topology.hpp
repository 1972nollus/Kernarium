// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uml::platform {

/* Matches the host query used by the maintained UML kernel when an explicit
 * ncpus= argument is absent.
 */
[[nodiscard]] unsigned online_processor_count() noexcept;

inline constexpr unsigned maximum_host_performance_levels = 64U;
inline constexpr unsigned normalized_cpu_capacity = 1024U;

struct HostPerformanceLevel final {
	unsigned logical_cpus{};
	unsigned maximum_logical_cpus{};
};

struct HostPerformanceTopology final {
	std::vector<HostPerformanceLevel> levels;
};

using HostTopologyIntegerQuery = std::optional<unsigned> (*)(
	void *context, std::string_view name) noexcept;

/* The callback form keeps discovery deterministic in host-only unit tests.
 * Production uses the public Apple sysctls through the no-argument wrapper.
 */
[[nodiscard]] std::optional<HostPerformanceTopology>
discover_host_performance_topology(
	HostTopologyIntegerQuery query, void *context);
[[nodiscard]] std::optional<HostPerformanceTopology>
host_performance_topology();

struct GuestCpuCapacityPlan final {
	std::vector<unsigned> capacities;
	std::vector<unsigned> performance_levels;

	[[nodiscard]] bool asymmetric() const noexcept;
};

[[nodiscard]] std::optional<std::vector<unsigned>>
parse_performance_level_capacities(std::string_view text);
[[nodiscard]] std::optional<GuestCpuCapacityPlan>
make_guest_cpu_capacity_plan(
	const HostPerformanceTopology &topology, unsigned guest_cpus,
	const std::vector<unsigned> &level_capacities);
[[nodiscard]] std::string format_guest_cpu_capacities(
	const GuestCpuCapacityPlan &plan);

} // namespace uml::platform
