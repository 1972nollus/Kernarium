// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_flag_test_support.hpp"

#include <array>
#include <cstdint>

namespace uml::backend::tests::base_flag {
namespace {

constexpr std::array<std::uint64_t, 28> values{
	0,
	1,
	2,
	0x7f,
	0x80,
	0x81,
	0xff,
	0x100,
	0x17f,
	0x180,
	0x1ff,
	0x7fff,
	0x8000,
	0x8001,
	0xffff,
	0x1'0000,
	0x1'7fff,
	0x1'8000,
	0x1'ffff,
	0xffff'ffffULL,
	0x1'0000'0000ULL,
	0x0123'4567'89ab'0000ULL,
	0x0123'4567'89ab'0080ULL,
	0x0123'4567'89ab'8000ULL,
	0x8000'0000'0000'0000ULL,
	0xffff'ffff'0000'0000ULL,
	0xaaaa'aaaa'aaaa'aaaaULL,
	0xffff'ffff'ffff'ffffULL,
};

void sign_zero_overflow_and_carry(EngineHarness &harness)
{
	for (const bool sixteen : {false, true})
		for (unsigned flags = 0; flags < 16U; ++flags)
			for (unsigned value_index = 0; value_index < values.size();
			     ++value_index) {
				const auto instruction = encode_setf(sixteen, 13);
				auto initial = seeded_state(
					0x3400'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(sixteen) << 55U) ^
					(static_cast<std::uint64_t>(flags) << 48U) ^
					value_index);
				initial.registers[13] = values[value_index];
				initial.nzcv = flags << 28U;
				char name[144];
				std::snprintf(name, sizeof(name),
					"BASE-0363 SETF%u flags=%x value-index=%u",
					sixteen ? 16U : 8U, flags, value_index);
				check(harness, name, instruction, initial);
			}
}

void zero_register_behavior(EngineHarness &harness)
{
	for (const bool sixteen : {false, true})
		for (unsigned flags = 0; flags < 16U; ++flags) {
			const auto instruction = encode_setf(sixteen, 31);
			auto initial = seeded_state(
				0x3410'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(flags) << 44U));
			initial.nzcv = flags << 28U;
			check(harness, "BASE-0363 SETF zero register",
				instruction, initial);
		}
}

} // namespace
} // namespace uml::backend::tests::base_flag

int main()
{
	uml::backend::tests::EngineHarness harness;
	uml::backend::tests::base_flag::sign_zero_overflow_and_carry(harness);
	uml::backend::tests::base_flag::zero_register_behavior(harness);
	harness.print_summary("arm64-base-flag-setf");
	return 0;
}
