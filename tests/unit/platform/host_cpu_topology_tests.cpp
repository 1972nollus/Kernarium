// SPDX-License-Identifier: GPL-2.0-or-later
#include "host_topology.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Value = std::pair<std::string_view, unsigned>;

struct QueryFixture final {
	std::vector<Value> values;

	static std::optional<unsigned> query(
		void *context, std::string_view name) noexcept
	{
		const auto &fixture = *static_cast<QueryFixture *>(context);
		for (const auto &[candidate, value] : fixture.values)
			if (candidate == name)
				return value;
		return std::nullopt;
	}
};

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error{message};
}

QueryFixture topology_fixture(
	std::initializer_list<unsigned> active,
	std::initializer_list<unsigned> maximum)
{
	QueryFixture fixture;
	unsigned active_total = 0;
	unsigned maximum_total = 0;
	fixture.values.emplace_back("hw.nperflevels", active.size());
	unsigned level = 0;
	for (const auto count : active) {
		static constexpr std::array active_names{
			std::string_view{"hw.perflevel0.logicalcpu"},
			std::string_view{"hw.perflevel1.logicalcpu"},
			std::string_view{"hw.perflevel2.logicalcpu"},
		};
		require(level < active_names.size(), "fixture has too many levels");
		fixture.values.emplace_back(active_names[level++], count);
		active_total += count;
	}
	level = 0;
	for (const auto count : maximum) {
		static constexpr std::array maximum_names{
			std::string_view{"hw.perflevel0.logicalcpu_max"},
			std::string_view{"hw.perflevel1.logicalcpu_max"},
			std::string_view{"hw.perflevel2.logicalcpu_max"},
		};
		require(level < maximum_names.size(), "fixture has too many levels");
		fixture.values.emplace_back(maximum_names[level++], count);
		maximum_total += count;
	}
	fixture.values.emplace_back("hw.logicalcpu", active_total);
	fixture.values.emplace_back("hw.logicalcpu_max", maximum_total);
	return fixture;
}

void discovery_accepts_one_two_and_three_levels()
{
	for (auto fixture : {
		topology_fixture({8}, {8}),
		topology_fixture({2, 4}, {2, 4}),
		topology_fixture({2, 4, 2}, {2, 4, 2}),
	}) {
		const auto topology =
			uml::platform::discover_host_performance_topology(
				QueryFixture::query, &fixture);
		require(topology.has_value(), "valid topology was rejected");
		require(topology->levels.size() ==
			fixture.values.front().second,
			"performance-level count changed");
	}
}

void discovery_fails_closed()
{
	auto missing = topology_fixture({2, 4}, {2, 4});
	missing.values.pop_back();
	require(!uml::platform::discover_host_performance_topology(
		QueryFixture::query, &missing),
		"missing global maximum was accepted");

	auto zero = topology_fixture({2, 4}, {2, 4});
	zero.values[2].second = 0;
	require(!uml::platform::discover_host_performance_topology(
		QueryFixture::query, &zero),
		"zero enabled level was accepted");

	auto inconsistent = topology_fixture({2, 4}, {2, 4});
	for (auto &[name, value] : inconsistent.values)
		if (name == "hw.logicalcpu")
			++value;
	require(!uml::platform::discover_host_performance_topology(
		QueryFixture::query, &inconsistent),
		"inconsistent active total was accepted");

	auto above_maximum = topology_fixture({2, 4}, {2, 4});
	for (auto &[name, value] : above_maximum.values)
		if (name == "hw.perflevel1.logicalcpu_max")
			value = 3;
	require(!uml::platform::discover_host_performance_topology(
		QueryFixture::query, &above_maximum),
		"enabled level above its maximum was accepted");
}

void discovery_reads_current_enabled_counts()
{
	auto fixture = topology_fixture({2, 4}, {2, 4});
	const auto first = uml::platform::discover_host_performance_topology(
		QueryFixture::query, &fixture);
	require(first && first->levels[0].logical_cpus == 2 &&
		first->levels[1].logical_cpus == 4,
		"initial enabled counts changed");
	for (auto &[name, value] : fixture.values) {
		if (name == "hw.perflevel0.logicalcpu")
			value = 1;
		else if (name == "hw.perflevel1.logicalcpu")
			value = 3;
		else if (name == "hw.logicalcpu")
			value = 4;
	}
	const auto second = uml::platform::discover_host_performance_topology(
		QueryFixture::query, &fixture);
	require(second && second->levels[0].logical_cpus == 1 &&
		second->levels[1].logical_cpus == 3,
		"discovery cached stale enabled counts");
}

void capacity_configuration_is_strict()
{
	const auto valid =
		uml::platform::parse_performance_level_capacities("1024,768,512");
	require(valid && *valid == std::vector<unsigned>({1024, 768, 512}),
		"valid capacity list changed");
	for (const auto invalid : {
		"", "1023,512", "1024,", "1024,,512", "1024,1024",
		"1024,0", "1024,1025", "1024,512x", "1024,768,800",
	})
		require(!uml::platform::parse_performance_level_capacities(invalid),
			"malformed capacity list was accepted");
}

void guest_plan_assigns_fastest_levels_first()
{
	auto fixture = topology_fixture({2, 4, 2}, {2, 4, 2});
	const auto topology = uml::platform::discover_host_performance_topology(
		QueryFixture::query, &fixture);
	const std::vector<unsigned> capacities{1024, 700, 400};
	const auto four = uml::platform::make_guest_cpu_capacity_plan(
		*topology, 4, capacities);
	require(four &&
		four->capacities == std::vector<unsigned>({1024, 1024, 700, 700}) &&
		four->performance_levels == std::vector<unsigned>({0, 0, 1, 1}) &&
		four->asymmetric(),
		"four-vCPU plan did not fill the fastest level first");
	const auto two = uml::platform::make_guest_cpu_capacity_plan(
		*topology, 2, capacities);
	require(two && !two->asymmetric() &&
		two->capacities == std::vector<unsigned>({1024, 1024}),
		"guest contained by level zero became asymmetric");
	require(!uml::platform::make_guest_cpu_capacity_plan(
		*topology, 9, capacities),
		"guest larger than enabled topology was accepted");
	require(!uml::platform::make_guest_cpu_capacity_plan(
		*topology, 4, std::vector<unsigned>{1024, 512}),
		"topology without a capacity for every level was accepted");
	require(uml::platform::format_guest_cpu_capacities(*four) ==
		"darwin_cpu_capacity_v1=1024,1024,700,700",
		"kernel capacity argument changed");
}

} // namespace

int main()
{
	try {
		discovery_accepts_one_two_and_three_levels();
		discovery_fails_closed();
		discovery_reads_current_enabled_counts();
		capacity_configuration_is_strict();
		guest_plan_assigns_fastest_levels_first();
		std::puts("host CPU topology tests passed");
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		return std::fprintf(stderr, "host CPU topology test failed: %s\n",
			error.what()), EXIT_FAILURE;
	}
}
