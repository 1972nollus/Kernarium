// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_bit_test_support.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace test = base_bit_test;
namespace engine_tests = uml::backend::tests;

namespace {

[[noreturn]] void fail(const char *message)
{
	std::fprintf(stderr, "%s\n", message);
	std::abort();
}

void require(bool condition, const char *message)
{
	if (!condition)
		fail(message);
}

void check(engine_tests::EngineHarness &harness, unsigned source,
	unsigned destination, std::uint64_t value, std::size_t &cases)
{
	auto initial = test::state(
		0x4300'0000'0000'0000ULL ^
		(static_cast<std::uint64_t>(source) << 24U) ^
		(static_cast<std::uint64_t>(destination) << 16U) ^ value);
	if (source != 31U)
		initial.registers[source] = value;
	char name[128];
	std::snprintf(name, sizeof(name),
		"BASE-0350 REV64 rn=%u rd=%u case=%zu",
		source, destination, cases);
	harness.check_one(name,
		test::instruction(true, 3U, source, destination), initial,
		test::expected_state(initial, true, 3U, source, destination),
		engine_tests::NativeRequirements::baseline());
	++cases;
}

void every_register_field(
	engine_tests::EngineHarness &harness, std::size_t &cases)
{
	for (unsigned source = 0; source < 32U; ++source)
		for (unsigned destination = 0; destination < 32U; ++destination)
			check(harness, source, destination,
				0x0123'4567'89ab'cdefULL ^
					(static_cast<std::uint64_t>(source) << 40U) ^
					(static_cast<std::uint64_t>(destination) << 8U),
				cases);
}

void boundary_values(
	engine_tests::EngineHarness &harness, std::size_t &cases)
{
	constexpr std::array values{
		std::uint64_t{0},
		std::uint64_t{1},
		std::uint64_t{0xff},
		std::uint64_t{0x100},
		std::uint64_t{0xffff},
		std::uint64_t{0x1'0000},
		std::uint64_t{0xffff'ffffULL},
		std::uint64_t{0x1'0000'0000ULL},
		std::uint64_t{0x0123'4567'89ab'cdefULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'ffffULL},
	};
	constexpr std::array register_modes{
		std::array{7U, 11U},
		std::array{7U, 7U},
		std::array{31U, 11U},
		std::array{7U, 31U},
		std::array{31U, 31U},
	};
	for (const auto value : values)
		for (const auto mode : register_modes)
			check(harness, mode[0], mode[1], value, cases);
}

} // namespace

int main()
{
	engine_tests::EngineHarness harness;
	std::size_t cases = 0;
	every_register_field(harness, cases);
	boundary_values(harness, cases);
	require(cases == 1'079U, "REV64 alias case count mismatch");
	harness.print_summary("arm64-base-rev64-alias");
	return 0;
}
