// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_crc_test_support.hpp"

#include <array>
#include <cstdint>

namespace uml::backend::tests::base_crc {
namespace {

struct Registers final {
	unsigned accumulator;
	unsigned source;
	unsigned destination;
	const char *field;
};

void every_register_field(EngineHarness &harness)
{
	for (unsigned operation = 0; operation < 8; ++operation)
		for (unsigned reg = 0; reg < 32; ++reg) {
			const std::array patterns{
				Registers{reg, 11, 23, "accumulator"},
				Registers{7, reg, 23, "source"},
				Registers{7, 11, reg, "destination"},
				Registers{reg, reg, reg, "all-alias"},
			};
			for (unsigned pattern = 0; pattern < patterns.size();
			     ++pattern) {
				const auto &registers = patterns[pattern];
				const auto instruction = encode(
					operation, registers.accumulator,
					registers.source,
					registers.destination);
				auto initial = seeded_state(
					0x2100'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(
						instruction) << 7) ^
					pattern);
				char name[144];
				std::snprintf(name, sizeof(name),
					"checksum operation=%u %s register=%u",
					operation, registers.field, reg);
				check(harness, name, instruction, initial);
			}
		}
}

void every_zero_discard_form(EngineHarness &harness)
{
	for (unsigned operation = 0; operation < 8; ++operation)
		for (const bool accumulator_zero : {false, true})
			for (const bool source_zero : {false, true})
				for (const bool discard : {false, true}) {
					const auto instruction = encode(
						operation,
						accumulator_zero ? 31U : 7U,
						source_zero ? 31U : 11U,
						discard ? 31U : 23U);
					auto initial = seeded_state(
						0x2110'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(
							instruction) << 9));
					char name[144];
					std::snprintf(name, sizeof(name),
						"checksum operation=%u zero/discard "
						"form=%u%u%u",
						operation,
						static_cast<unsigned>(
							accumulator_zero),
						static_cast<unsigned>(
							source_zero),
						static_cast<unsigned>(discard));
					check(harness, name, instruction, initial);
				}
}

} // namespace
} // namespace uml::backend::tests::base_crc

int main()
{
	uml::backend::tests::EngineHarness harness;
	uml::backend::tests::base_crc::every_register_field(harness);
	uml::backend::tests::base_crc::every_zero_discard_form(harness);
	constexpr std::size_t cases = 8U * 32U * 4U + 8U * 2U * 2U * 2U;
	uml::backend::tests::base_crc::require_engine_counts(harness, cases);
	harness.print_summary("arm64-base-crc-allocation");
	return 0;
}
