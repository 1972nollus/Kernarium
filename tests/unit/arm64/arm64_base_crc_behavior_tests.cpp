// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_crc_test_support.hpp"

#include <array>
#include <cstdint>

namespace uml::emulator::tests::base_crc {
namespace {

constexpr std::array<std::uint32_t, 10> accumulators{
	0,
	1,
	2,
	0x7fff'ffffU,
	0x8000'0000U,
	0xffff'ffffU,
	0x1234'5678U,
	0x89ab'cdefU,
	0xaaaa'aaaaU,
	0x5555'5555U,
};

constexpr std::array<std::uint64_t, 14> values{
	0,
	1,
	2,
	0x7f,
	0x80,
	0xff,
	0x100,
	0xffff,
	0x1'0000ULL,
	0xffff'ffffULL,
	0x1'0000'0000ULL,
	0x0123'4567'89ab'cdefULL,
	0x8000'0000'0000'0000ULL,
	0xffff'ffff'ffff'ffffULL,
};

void known_answers(EngineHarness &harness)
{
	for (unsigned operation = 0; operation < 8; ++operation)
		for (unsigned accumulator_index = 0;
		     accumulator_index < accumulators.size();
		     ++accumulator_index)
			for (unsigned value_index = 0;
			     value_index < values.size(); ++value_index) {
				const auto instruction =
					encode(operation, 7, 11, 23);
				auto initial = seeded_state(
					0x2200'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(
						operation) << 52) ^
					(static_cast<std::uint64_t>(
						accumulator_index) << 44) ^
					value_index);
				initial.registers[7] =
					0xfeed'face'0000'0000ULL |
					accumulators[accumulator_index];
				initial.registers[11] = values[value_index];
				char name[160];
				std::snprintf(name, sizeof(name),
					"checksum operation=%u accumulator=%#x "
					"value=%#llx",
					operation,
					accumulators[accumulator_index],
					static_cast<unsigned long long>(
						values[value_index]));
				check(harness, name, instruction, initial);
			}
}

void destructive_aliases(EngineHarness &harness)
{
	constexpr std::array patterns{
		std::array{7U, 11U, 7U},
		std::array{7U, 11U, 11U},
		std::array{7U, 7U, 23U},
		std::array{7U, 7U, 7U},
		std::array{31U, 7U, 7U},
		std::array{7U, 31U, 7U},
		std::array{31U, 31U, 7U},
	};
	for (unsigned operation = 0; operation < 8; ++operation)
		for (unsigned pattern = 0; pattern < patterns.size(); ++pattern) {
			const auto &registers = patterns[pattern];
			const auto instruction = encode(
				operation, registers[0], registers[1],
				registers[2]);
			auto initial = seeded_state(
				0x2210'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(instruction) << 11) ^
				pattern);
			initial.registers[7] = 0x0123'4567'89ab'cdefULL;
			initial.registers[11] = 0xfedc'ba98'7654'3210ULL;
			char name[144];
			std::snprintf(name, sizeof(name),
				"checksum operation=%u destructive alias=%u",
				operation, pattern);
			check(harness, name, instruction, initial);
		}
}

void ignored_high_bits(EngineHarness &harness)
{
	for (unsigned operation = 0; operation < 8; ++operation) {
		const auto instruction = encode(operation, 7, 11, 23);
		const unsigned bytes = 1U << (operation & 3U);
		const auto source_mask = bytes == 8U ?
			~std::uint64_t{0} :
			(std::uint64_t{1} << (bytes * 8U)) - 1U;
		auto low = seeded_state(
			0x2220'0000'0000'0000ULL ^ operation);
		low.registers[7] = 0x0123'4567'89ab'cdefULL;
		low.registers[11] = 0x8877'6655'4433'2211ULL;
		auto high = low;
		high.registers[7] ^= 0xffff'ffff'0000'0000ULL;
		high.registers[11] =
			(low.registers[11] & source_mask) |
			(~low.registers[11] & ~source_mask);
		check(harness, "checksum ignored accumulator high half",
			instruction, low);
		check(harness, "checksum ignored source high bytes",
			instruction, high);
	}
}

} // namespace
} // namespace uml::emulator::tests::base_crc

int main()
{
	uml::emulator::tests::EngineHarness harness;
	uml::emulator::tests::base_crc::known_answers(harness);
	uml::emulator::tests::base_crc::destructive_aliases(harness);
	uml::emulator::tests::base_crc::ignored_high_bits(harness);
	constexpr std::size_t cases =
		8U * 10U * 14U + 8U * 7U + 8U * 2U;
	uml::emulator::tests::base_crc::require_engine_counts(harness, cases);
	harness.print_summary("arm64-base-crc-behavior");
	return 0;
}
