// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_flag_test_support.hpp"

#include <array>
#include <cstdint>

namespace uml::backend::tests::base_flag {
namespace {

constexpr std::array<std::uint64_t, 16> values{
	0,
	1,
	2,
	3,
	0xf,
	0x10,
	0x80,
	0xff,
	0x8000'0000ULL,
	0xffff'ffffULL,
	0x1'0000'0000ULL,
	0x0123'4567'89ab'cdefULL,
	0x8000'0000'0000'0000ULL,
	0xf000'0000'0000'000fULL,
	0xaaaa'aaaa'aaaa'aaaaULL,
	0xffff'ffff'ffff'ffffULL,
};

void every_rotation(EngineHarness &harness)
{
	for (unsigned rotation = 0; rotation < 64U; ++rotation)
		for (unsigned value_index = 0; value_index < values.size();
		     ++value_index) {
			const unsigned mask = (rotation + value_index) & 15U;
			const auto instruction = encode_rmif(7, rotation, mask);
			auto initial = seeded_state(
				0x3300'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(rotation) << 48U) ^
				value_index);
			initial.registers[7] = values[value_index];
			initial.nzcv =
				((rotation * 3U + value_index) & 15U) << 28U;
			char name[144];
			std::snprintf(name, sizeof(name),
				"BASE-0351 RMIF rotation=%u value-index=%u mask=%u",
				rotation, value_index, mask);
			check(harness, name, instruction, initial);
		}
}

void every_mask_and_preserved_flag(EngineHarness &harness)
{
	constexpr std::array<std::uint64_t, 4> mask_values{
		0,
		0xf,
		0x0123'4567'89ab'cdefULL,
		0xfedc'ba98'7654'3210ULL,
	};
	for (unsigned mask = 0; mask < 16U; ++mask)
		for (unsigned flags = 0; flags < 16U; ++flags)
			for (unsigned value_index = 0;
			     value_index < mask_values.size(); ++value_index) {
				const unsigned rotation =
					(mask * 13U + flags * 7U +
						value_index * 17U) &
					63U;
				const auto instruction =
					encode_rmif(19, rotation, mask);
				auto initial = seeded_state(
					0x3310'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(
						instruction) << 5U) ^
					flags);
				initial.registers[19] = mask_values[value_index];
				initial.nzcv = flags << 28U;
				char name[160];
				std::snprintf(name, sizeof(name),
					"BASE-0351 RMIF mask=%u initial=%x "
					"rotation=%u value-index=%u",
					mask, flags, rotation, value_index);
				check(harness, name, instruction, initial);
			}
}

void zero_register_behavior(EngineHarness &harness)
{
	for (unsigned mask = 0; mask < 16U; ++mask)
		for (unsigned flags = 0; flags < 16U; ++flags) {
			const unsigned rotation = (mask * 11U + flags * 5U) & 63U;
			const auto instruction = encode_rmif(31, rotation, mask);
			auto initial = seeded_state(
				0x3320'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(instruction) << 7U));
			initial.nzcv = flags << 28U;
			check(harness, "BASE-0351 RMIF zero register",
				instruction, initial);
		}
}

} // namespace
} // namespace uml::backend::tests::base_flag

int main()
{
	uml::backend::tests::EngineHarness harness;
	uml::backend::tests::base_flag::every_rotation(harness);
	uml::backend::tests::base_flag::every_mask_and_preserved_flag(harness);
	uml::backend::tests::base_flag::zero_register_behavior(harness);
	harness.print_summary("arm64-base-flag-rmif");
	return 0;
}
