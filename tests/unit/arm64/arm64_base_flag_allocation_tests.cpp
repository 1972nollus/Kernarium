// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_flag_test_support.hpp"

#include <array>
#include <cstdint>

namespace uml::emulator::tests::base_flag {
namespace {

void fixed_encodings(EngineHarness &harness)
{
	constexpr std::array encodings{
		cfinv_encoding,
		axflag_encoding,
		xaflag_encoding,
	};
	for (const auto instruction : encodings)
		for (unsigned flags = 0; flags < 16U; ++flags) {
			auto initial = seeded_state(
				0x3100'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(instruction) << 9U) ^
				flags);
			initial.nzcv = flags << 28U;
			check(harness, "fixed flag allocation", instruction, initial);
		}
}

void every_rmif_field(EngineHarness &harness)
{
	for (unsigned source = 0; source < 32U; ++source)
		for (unsigned rotation = 0; rotation < 64U; ++rotation)
			for (unsigned mask = 0; mask < 16U; ++mask) {
				const auto instruction =
					encode_rmif(source, rotation, mask);
				auto initial = seeded_state(
					0x3110'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(
						instruction) << 13U));
				initial.nzcv =
					((source + rotation + mask) & 15U) << 28U;
				char name[144];
				std::snprintf(name, sizeof(name),
					"BASE-0351 RMIF source=%u rotation=%u mask=%u",
					source, rotation, mask);
				check(harness, name, instruction, initial);
			}
}

void every_setf_field(EngineHarness &harness)
{
	for (const bool sixteen : {false, true})
		for (unsigned source = 0; source < 32U; ++source) {
			const auto instruction = encode_setf(sixteen, source);
			auto initial = seeded_state(
				0x3120'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(instruction) << 11U));
			initial.nzcv = ((source + static_cast<unsigned>(sixteen)) &
				15U) << 28U;
			char name[112];
			std::snprintf(name, sizeof(name),
				"BASE-0363 SETF%u source=%u",
				sixteen ? 16U : 8U, source);
			check(harness, name, instruction, initial);
		}
}

} // namespace
} // namespace uml::emulator::tests::base_flag

int main()
{
	uml::emulator::tests::EngineHarness harness;
	uml::emulator::tests::base_flag::fixed_encodings(harness);
	uml::emulator::tests::base_flag::every_rmif_field(harness);
	uml::emulator::tests::base_flag::every_setf_field(harness);
	harness.print_summary("arm64-base-flag-allocation");
	return 0;
}
