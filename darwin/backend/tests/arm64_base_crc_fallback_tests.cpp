// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_crc_test_support.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

namespace uml::backend::tests::base_crc {
namespace {

constexpr std::array<std::uint32_t, 4> accumulators{
	0U, 1U, 0x8000'0000U, 0x89ab'cdefU,
};
constexpr std::array<std::uint64_t, 4> values{
	0U, 0xffU, 0x0123'4567'89ab'cdefULL, 0xffff'ffff'ffff'ffffULL,
};
constexpr std::array source_modes{
	std::array{7U, 11U},
	std::array{7U, 7U},
	std::array{7U, 31U},
	std::array{31U, 11U},
	std::array{31U, 31U},
};

void every_register_field(EngineHarness &harness)
{
	for (unsigned operation = 0; operation < 8U; ++operation)
		for (unsigned reg = 0; reg < 32U; ++reg) {
			const std::array registers{
				std::array{reg, 11U, 23U},
				std::array{7U, reg, 23U},
				std::array{7U, 11U, reg},
				std::array{reg, reg, reg},
			};
			for (unsigned pattern = 0; pattern < registers.size();
			     ++pattern) {
				const auto instruction = encode(operation,
					registers[pattern][0], registers[pattern][1],
					registers[pattern][2]);
				const auto initial = seeded_state(
					0x23f0'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(instruction) << 7) ^
					pattern);
				char name[144];
				std::snprintf(name, sizeof(name),
					"forced software checksum operation=%u "
					"register=%u pattern=%u",
					operation, reg, pattern);
				check(harness, name, instruction, initial);
			}
		}
}

void every_operation_source_mode_and_value(EngineHarness &harness)
{
	for (unsigned operation = 0; operation < 8U; ++operation)
		for (unsigned mode = 0; mode < source_modes.size(); ++mode)
			for (unsigned sample = 0; sample < values.size(); ++sample) {
				const auto accumulator = source_modes[mode][0];
				const auto source = source_modes[mode][1];
				const auto instruction =
					encode(operation, accumulator, source, 23U);
				auto initial = seeded_state(
					0x2400'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(instruction) << 9) ^
					sample);
				if (accumulator != 31U)
					initial.registers[accumulator] =
						0xfeed'face'0000'0000ULL |
						accumulators[sample];
				if (source != 31U)
					initial.registers[source] = values[sample];
				char name[144];
				std::snprintf(name, sizeof(name),
					"forced software checksum operation=%u "
					"source-mode=%u sample=%u",
					operation, mode, sample);
				check(harness, name, instruction, initial);
			}
}

void every_destructive_alias(EngineHarness &harness)
{
	constexpr std::array aliases{
		std::array{7U, 11U, 7U},
		std::array{7U, 11U, 11U},
		std::array{7U, 7U, 23U},
		std::array{7U, 7U, 7U},
		std::array{31U, 7U, 7U},
		std::array{7U, 31U, 7U},
		std::array{31U, 31U, 7U},
	};
	for (unsigned operation = 0; operation < 8U; ++operation)
		for (unsigned alias = 0; alias < aliases.size(); ++alias) {
			const auto instruction = encode(operation,
				aliases[alias][0], aliases[alias][1], aliases[alias][2]);
			auto initial = seeded_state(
				0x2410'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(instruction) << 11) ^
				alias);
			initial.registers[7] = 0x0123'4567'89ab'cdefULL;
			initial.registers[11] = 0xfedc'ba98'7654'3210ULL;
			char name[128];
			std::snprintf(name, sizeof(name),
				"forced software checksum operation=%u alias=%u",
				operation, alias);
			check(harness, name, instruction, initial);
		}
}

} // namespace
} // namespace uml::backend::tests::base_crc

int main()
{
	uml::backend::tests::EngineHarness harness{
		uml::backend::tests::EngineHarnessOptions{
			.force_software_checksum = true,
		}};
	uml::backend::tests::base_crc::every_register_field(harness);
	uml::backend::tests::base_crc::every_operation_source_mode_and_value(
		harness);
	uml::backend::tests::base_crc::every_destructive_alias(harness);
	constexpr std::size_t cases =
		8U * 32U * 4U + 8U * 5U * 4U + 8U * 7U;
	const auto &counts = harness.counts();
	uml::backend::tests::base_crc::require(
		counts.interpreter == cases && counts.gadgets == cases &&
			counts.jit + counts.jit_skipped == cases,
		"forced software checksum engine case count mismatch");
	harness.print_summary("arm64-base-crc-fallback");
	return 0;
}
