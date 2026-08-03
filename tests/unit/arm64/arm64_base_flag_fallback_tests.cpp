// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_flag_test_support.hpp"

#include <array>
#include <cstdint>

namespace uml::emulator::tests::base_flag {
namespace {

constexpr std::array<std::uint64_t, 16> setf_values{
	0,
	1,
	0x7f,
	0x80,
	0xff,
	0x100,
	0x180,
	0x1ff,
	0x7fff,
	0x8000,
	0xffff,
	0x1'0000,
	0x1'8000,
	0x1'ffff,
	0x0123'4567'89ab'cdefULL,
	0xffff'ffff'ffff'ffffULL,
};

void fixed_portable_paths(EngineHarness &harness)
{
	constexpr std::array encodings{
		cfinv_encoding,
		axflag_encoding,
		xaflag_encoding,
	};
	for (const auto instruction : encodings)
		for (unsigned flags = 0; flags < 16U; ++flags) {
			auto initial = seeded_state(
				0x3600'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(instruction) << 9U) ^
				flags);
			initial.nzcv = flags << 28U;
			check(harness, "portable fixed flag path", instruction,
				initial);
		}
}

void setf_portable_paths(EngineHarness &harness)
{
	for (const bool sixteen : {false, true})
		for (unsigned source = 0; source < 32U; ++source)
			for (unsigned sample = 0; sample < setf_values.size();
			     ++sample) {
				const auto instruction = encode_setf(sixteen, source);
				auto initial = seeded_state(
					0x3610'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(
						instruction) << 11U) ^
					sample);
				if (source != 31U)
					initial.registers[source] =
						setf_values[sample];
				initial.nzcv =
					((source + sample) & 15U) << 28U;
				check(harness, "portable SETF path", instruction,
					initial);
			}
}

} // namespace
} // namespace uml::emulator::tests::base_flag

int main()
{
	uml::emulator::tests::EngineHarness harness{
		uml::emulator::tests::EngineHarnessOptions{
			.force_software_flags = true,
		}};
	uml::emulator::tests::base_flag::fixed_portable_paths(harness);
	uml::emulator::tests::base_flag::setf_portable_paths(harness);
	constexpr std::size_t cases = 3U * 16U + 2U * 32U * 16U;
	const auto &counts = harness.counts();
	uml::emulator::tests::base_flag::require(
		counts.interpreter == cases && counts.gadgets == cases &&
			counts.jit + counts.jit_skipped == cases,
		"portable flag engine case count mismatch");
	harness.print_summary("arm64-base-flag-fallback");
	return 0;
}
