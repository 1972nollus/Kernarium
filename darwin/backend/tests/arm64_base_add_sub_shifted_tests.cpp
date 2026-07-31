// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_add_sub_test_support.hpp"

#include <array>
#include <cstdio>

namespace uml::backend::tests::base_add_sub {
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

void every_shift_field(EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool set_flags : {false, true})
				for (unsigned shift_type = 0; shift_type < 3;
				     ++shift_type)
					for (unsigned amount = 0;
					     amount < (wide ? 64U : 32U); ++amount)
						for (const auto &registers :
						     register_patterns) {
							const auto instruction = encode_shifted(
								wide, subtract, set_flags,
								shift_type, amount,
								registers.left, registers.right,
								registers.destination);
							auto initial = seeded_state(
								0x6000'0000'0000'0000ULL ^
								(static_cast<std::uint64_t>(
									instruction) << 17) ^
								amount);
							char name[144];
							std::snprintf(name, sizeof(name),
								"shifted width=%u op=%u flags=%u "
								"type=%u amount=%u %.*s",
								wide ? 64U : 32U,
								subtract ? 1U : 0U,
								set_flags ? 1U : 0U,
								shift_type, amount,
								static_cast<int>(
									registers.name.size()),
								registers.name.data());
							check(harness, name,
								Form::shifted_register,
								instruction, initial);
						}
}

void arithmetic_boundaries(EngineHarness &harness)
{
	const std::array pairs{
		std::array{std::uint64_t{0}, std::uint64_t{0}},
		std::array{std::uint64_t{0}, std::uint64_t{1}},
		std::array{std::uint64_t{1}, std::uint64_t{1}},
		std::array{std::uint64_t{0x7fff'ffffU},
			std::uint64_t{1}},
		std::array{std::uint64_t{0x8000'0000U},
			std::uint64_t{0xffff'ffffU}},
		std::array{std::uint64_t{0xffff'ffffU},
			std::uint64_t{1}},
		std::array{std::uint64_t{0x7fff'ffff'ffff'ffffULL},
			std::uint64_t{1}},
		std::array{std::uint64_t{0x8000'0000'0000'0000ULL},
			std::uint64_t{0xffff'ffff'ffff'ffffULL}},
		std::array{std::uint64_t{0xffff'ffff'ffff'ffffULL},
			std::uint64_t{1}},
		std::array{std::uint64_t{0x0123'4567'89ab'cdefULL},
			std::uint64_t{0xfedc'ba98'7654'3210ULL}},
	};

	for (const bool wide : {false, true}) {
		const std::array amounts{
			0U, 1U, wide ? 32U : 16U, wide ? 63U : 31U};
		for (const bool subtract : {false, true})
			for (const bool set_flags : {false, true})
				for (unsigned shift_type = 0; shift_type < 3;
				     ++shift_type)
					for (const auto amount : amounts)
						for (unsigned pair_index = 0;
						     pair_index < pairs.size();
						     ++pair_index) {
							auto initial = seeded_state(
								0x7000'0000'0000'0000ULL ^
								(static_cast<std::uint64_t>(
									pair_index) << 48));
							initial.registers[13] =
								pairs[pair_index][0];
							initial.registers[27] =
								pairs[pair_index][1];
							char name[144];
							std::snprintf(name, sizeof(name),
								"shifted boundary width=%u op=%u "
								"flags=%u type=%u amount=%u pair=%u",
								wide ? 64U : 32U,
								subtract ? 1U : 0U,
								set_flags ? 1U : 0U,
								shift_type, amount, pair_index);
							check(harness, name,
								Form::shifted_register,
								encode_shifted(wide, subtract,
									set_flags, shift_type,
									amount, 13, 27, 19),
								initial);
						}
	}
}

void every_register_number(EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool set_flags : {false, true})
				for (unsigned reg = 0; reg < 32; ++reg) {
					const std::array patterns{
						Registers{reg, 11, 23, "rn"},
						Registers{11, reg, 23, "rm"},
						Registers{11, 23, reg, "rd"},
						Registers{reg, reg, reg, "all"},
					};
					for (const auto &registers : patterns) {
						auto initial = seeded_state(
							0x7100'0000'0000'0000ULL ^
							(static_cast<std::uint64_t>(reg) << 32));
						char name[112];
						std::snprintf(name, sizeof(name),
							"shifted register sweep width=%u "
							"op=%u flags=%u reg=%u %.*s",
							wide ? 64U : 32U,
							subtract ? 1U : 0U,
							set_flags ? 1U : 0U, reg,
							static_cast<int>(
								registers.name.size()),
							registers.name.data());
						check(harness, name,
							Form::shifted_register,
							encode_shifted(wide, subtract,
								set_flags, reg % 3U,
								reg % (wide ? 64U : 32U),
								registers.left,
								registers.right,
								registers.destination),
							initial);
					}
				}
}

void aliases(EngineHarness &harness)
{
	/*
	 * The register patterns above cover all aliases for every shift field.
	 * These named cases make their flag/discard contracts explicit.
	 */
	for (const bool wide : {false, true})
		for (unsigned shift_type = 0; shift_type < 3; ++shift_type)
			for (const unsigned amount :
			     {0U, 1U, wide ? 63U : 31U}) {
				for (const bool flags : {false, true}) {
					auto initial = seeded_state(
						0x7200'0000'0000'0000ULL |
						(static_cast<std::uint64_t>(amount) << 40));
					initial.registers[27] =
						0x8000'0000'0000'0001ULL;
					char name[96];
					std::snprintf(name, sizeof(name),
						"%s width=%u type=%u amount=%u",
						flags ? "NEGS" : "NEG",
						wide ? 64U : 32U,
						shift_type, amount);
					check(harness, name, Form::shifted_register,
						encode_shifted(wide, true, flags,
							shift_type, amount, 31, 27, 13),
						initial);
				}
				for (const bool subtract : {false, true}) {
					auto initial = seeded_state(
						0x7300'0000'0000'0000ULL |
						(static_cast<std::uint64_t>(amount) << 40));
					char name[96];
					std::snprintf(name, sizeof(name),
						"%s shifted width=%u type=%u amount=%u",
						subtract ? "CMP" : "CMN",
						wide ? 64U : 32U,
						shift_type, amount);
					check(harness, name, Form::shifted_register,
						encode_shifted(wide, subtract, true,
							shift_type, amount, 13, 27, 31),
						initial);
				}
			}
}

void reserved_encodings(EngineHarness &harness)
{
	/* shift=='11' is reserved for both register widths. */
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool set_flags : {false, true})
				for (unsigned amount = 0; amount < 64; ++amount) {
					const unsigned left = amount & 31U;
					const unsigned right = (amount * 7U) & 31U;
					const unsigned destination =
						(amount * 13U) & 31U;
					const auto instruction = encode_shifted(
						wide, subtract, set_flags, 3, amount,
						left, right, destination);
					auto initial = seeded_state(
						0x7400'0000'0000'0000ULL ^
						instruction);
					char name[112];
					std::snprintf(name, sizeof(name),
						"reserved shifted type width=%u op=%u "
						"flags=%u amount=%u",
						wide ? 64U : 32U,
						subtract ? 1U : 0U,
						set_flags ? 1U : 0U, amount);
					check_illegal(harness, name,
						Form::shifted_register,
						instruction, initial);
				}

	/* imm6[5]=='1' is reserved in every 32-bit shifted form. */
	for (const bool subtract : {false, true})
		for (const bool set_flags : {false, true})
			for (unsigned shift_type = 0; shift_type < 3;
			     ++shift_type)
				for (unsigned amount = 32; amount < 64; ++amount) {
					const auto instruction = encode_shifted(
						false, subtract, set_flags, shift_type,
						amount, amount & 31U,
						(amount + 11U) & 31U,
						(amount + 23U) & 31U);
					auto initial = seeded_state(
						0x7500'0000'0000'0000ULL ^
						instruction);
					char name[112];
					std::snprintf(name, sizeof(name),
						"reserved W shifted amount op=%u "
						"flags=%u type=%u amount=%u",
						subtract ? 1U : 0U,
						set_flags ? 1U : 0U,
						shift_type, amount);
					check_illegal(harness, name,
						Form::shifted_register,
						instruction, initial);
				}
}

} // namespace

void run_shifted_tests(EngineHarness &harness)
{
	every_shift_field(harness);
	arithmetic_boundaries(harness);
	every_register_number(harness);
	aliases(harness);
	reserved_encodings(harness);
}

} // namespace uml::backend::tests::base_add_sub
