// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_bit_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = base_bit_test;
namespace engine_tests = uml::emulator::tests;

namespace {

struct RegisterMode final {
	unsigned source;
	unsigned destination;
};

constexpr std::array register_modes{
	RegisterMode{1, 7},
	RegisterMode{7, 7},
	RegisterMode{31, 7},
	RegisterMode{1, 31},
	RegisterMode{30, 0},
};

void check_value(engine_tests::EngineHarness &harness, bool wide,
	unsigned operation, std::uint64_t value, RegisterMode mode,
	unsigned value_id)
{
	auto initial = test::state(value ^
		(static_cast<std::uint64_t>(operation) << 48U) ^
		(wide ? 0x8000'0000'0000'0000ULL : 0));
	if (mode.source != 31U)
		initial.registers[mode.source] = value;
	char name[128];
	std::snprintf(name, sizeof(name),
		"base-bit-behavior width=%u op=%u value=%u rn=%u rd=%u",
		wide ? 64U : 32U, operation, value_id, mode.source,
		mode.destination);
	harness.check_one(name,
		test::instruction(wide, operation, mode.source, mode.destination),
		initial,
		test::expected_state(initial, wide, operation, mode.source,
			mode.destination),
		test::native_requirement(operation));
}

void boundary_patterns(engine_tests::EngineHarness &harness)
{
	constexpr std::array values{
		std::uint64_t{0},
		std::uint64_t{1},
		std::uint64_t{2},
		std::uint64_t{3},
		std::uint64_t{0x7f},
		std::uint64_t{0x80},
		std::uint64_t{0xff},
		std::uint64_t{0x8000},
		std::uint64_t{0xffff},
		std::uint64_t{0x7fff'ffffU},
		std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x0123'4567'89ab'cdefULL},
		std::uint64_t{0x5555'5555'5555'5555ULL},
		std::uint64_t{0xaaaa'aaaa'aaaa'aaaaULL},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		~std::uint64_t{0},
	};
	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation < 9U; ++operation) {
			if (!test::allocated(wide, operation))
				continue;
			for (unsigned index = 0; index < values.size(); ++index)
				for (const auto mode : register_modes)
					check_value(harness, wide, operation,
						values[index], mode, index);
		}
}

void every_bit_position(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true}) {
		const unsigned width = wide ? 64U : 32U;
		for (unsigned operation = 0; operation < 9U; ++operation) {
			if (!test::allocated(wide, operation))
				continue;
			for (unsigned bit = 0; bit < width; ++bit) {
				const auto one = std::uint64_t{1} << bit;
				for (const auto mode : register_modes) {
					check_value(harness, wide, operation, one,
						mode, 100U + bit);
					check_value(harness, wide, operation,
						test::width_mask(wide) ^ one,
						mode, 200U + bit);
				}
			}
		}
	}
}

} // namespace

int main()
{
	engine_tests::EngineHarness harness;
	boundary_patterns(harness);
	every_bit_position(harness);
	harness.print_summary("arm64-base-bit-behavior");
	return 0;
}
