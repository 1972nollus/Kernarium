// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_logical_test_support.hpp"

#include <array>
#include <cstdio>

namespace uml::emulator::tests::base_logical {
namespace {

struct Registers final {
	unsigned left;
	unsigned right;
	unsigned destination;
	std::string_view name;
};

constexpr std::array register_patterns{
	Registers{1, 2, 7, "ordinary"},
	Registers{30, 29, 28, "high"},
	Registers{7, 2, 7, "rd-rn"},
	Registers{1, 7, 7, "rd-rm"},
	Registers{7, 7, 2, "rn-rm"},
	Registers{7, 7, 7, "all-alias"},
	Registers{31, 2, 7, "zero-left"},
	Registers{1, 31, 7, "zero-right"},
	Registers{1, 2, 31, "discard"},
	Registers{31, 31, 7, "both-zero"},
	Registers{31, 2, 31, "zero-left-discard"},
	Registers{1, 31, 31, "zero-right-discard"},
};

void every_valid_shift_field(EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation < 4; ++operation)
			for (const bool invert : {false, true})
				for (unsigned shift_type = 0; shift_type < 4;
				     ++shift_type)
					for (unsigned amount = 0;
					     amount < (wide ? 64U : 32U);
					     ++amount)
						for (const auto &registers :
						     register_patterns) {
							const auto instruction = encode_shifted(
								wide, operation, invert,
								shift_type, amount,
								registers.left,
								registers.right,
								registers.destination);
							auto initial = seeded_state(
								0x2000'0000'0000'0000ULL ^
								(static_cast<std::uint64_t>(
									instruction) << 19) ^
								amount);
							char name[160];
							std::snprintf(name, sizeof(name),
								"logical shifted width=%u op=%u "
								"N=%u type=%u amount=%u %.*s",
								wide ? 64U : 32U,
								operation,
								invert ? 1U : 0U,
								shift_type, amount,
								static_cast<int>(
									registers.name.size()),
								registers.name.data());
							check(harness, name,
								Form::shifted_register,
								instruction, initial);
						}
}

void every_register_number(EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation < 4; ++operation)
			for (const bool invert : {false, true})
				for (unsigned reg = 0; reg < 32; ++reg) {
					const std::array patterns{
						Registers{reg, 11, 23, "rn"},
						Registers{11, reg, 23, "rm"},
						Registers{11, 23, reg, "rd"},
						Registers{reg, reg, reg, "all"},
					};
					for (const auto &registers : patterns) {
						const unsigned amount =
							(reg * 11U) %
							(wide ? 64U : 32U);
						const auto instruction = encode_shifted(
							wide, operation, invert,
							reg & 3U, amount,
							registers.left,
							registers.right,
							registers.destination);
						auto initial = seeded_state(
							0x2100'0000'0000'0000ULL ^
							(static_cast<std::uint64_t>(
								reg) << 40) ^
							instruction);
						char name[144];
						std::snprintf(name, sizeof(name),
							"logical shifted register sweep "
							"width=%u op=%u N=%u reg=%u %.*s",
							wide ? 64U : 32U, operation,
							invert ? 1U : 0U, reg,
							static_cast<int>(
								registers.name.size()),
							registers.name.data());
						check(harness, name,
							Form::shifted_register,
							instruction, initial);
					}
				}
}

void every_reserved_shift_field(EngineHarness &harness)
{
	/*
	 * For every operation, inversion value, shift kind, and high imm6
	 * value, sf==0 with imm6[5]==1 is unallocated.  No shift kind itself
	 * is reserved in the logical-register class.
	 */
	for (unsigned operation = 0; operation < 4; ++operation)
		for (const bool invert : {false, true})
			for (unsigned shift_type = 0; shift_type < 4; ++shift_type)
				for (unsigned amount = 32; amount < 64; ++amount) {
					const auto instruction = encode_shifted(
						false, operation, invert, shift_type,
						amount, amount & 31U,
						(amount * 7U + operation) & 31U,
						(amount * 13U +
						 static_cast<unsigned>(invert)) & 31U);
					auto initial = seeded_state(
						0x2200'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(
							instruction) << 21));
					char name[144];
					std::snprintf(name, sizeof(name),
						"reserved W logical shifted op=%u "
						"N=%u type=%u amount=%u",
						operation, invert ? 1U : 0U,
						shift_type, amount);
					check_illegal(harness, name,
						Form::shifted_register,
						instruction, initial);
				}
}

} // namespace

void run_shifted_tests(EngineHarness &harness)
{
	every_valid_shift_field(harness);
	every_register_number(harness);
	every_reserved_shift_field(harness);
}

} // namespace uml::emulator::tests::base_logical
