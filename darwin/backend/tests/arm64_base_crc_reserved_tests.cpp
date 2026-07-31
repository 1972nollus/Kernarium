// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_crc_test_support.hpp"

#include <array>
#include <cstdint>

namespace uml::backend::tests::base_crc {
namespace {

void wrong_widths(EngineHarness &harness)
{
	for (unsigned operation = 0; operation < 8; ++operation)
		for (unsigned reg = 0; reg < 32; ++reg) {
			const std::array instructions{
				encode(operation, reg, 11, 23, true),
				encode(operation, 7, reg, 23, true),
				encode(operation, 7, 11, reg, true),
				encode(operation, reg, reg, reg, true),
			};
			for (unsigned pattern = 0; pattern < instructions.size();
			     ++pattern) {
				auto initial = seeded_state(
					0x2300'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(
						instructions[pattern]) << 7) ^
					pattern);
				char name[128];
				std::snprintf(name, sizeof(name),
					"checksum reserved width operation=%u "
					"register=%u pattern=%u",
					operation, reg, pattern);
				check_illegal(harness, name,
					instructions[pattern], initial);
			}
		}
}

} // namespace
} // namespace uml::backend::tests::base_crc

int main()
{
	uml::backend::tests::EngineHarness harness;
	uml::backend::tests::base_crc::wrong_widths(harness);
	constexpr std::size_t cases = 8U * 32U * 4U;
	uml::backend::tests::base_crc::require_engine_counts(
		harness, cases, true);
	harness.print_summary("arm64-base-crc-reserved");
	return 0;
}
