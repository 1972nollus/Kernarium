// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_logical_test_support.hpp"

#include <array>
#include <cstdio>

namespace uml::backend::tests::base_logical {
namespace {

void immediate_boundaries(EngineHarness &harness)
{
	struct Encoding final {
		unsigned n;
		unsigned rotate;
		unsigned set_bits;
	};
	constexpr std::array encodings{
		Encoding{0, 0, 0},
		Encoding{0, 1, 0},
		Encoding{0, 7, 5},
		Encoding{0, 17, 23},
		Encoding{0, 31, 30},
	};
	constexpr std::array values{
		std::uint64_t{0},
		std::uint64_t{1},
		std::uint64_t{0x7fff'ffffU},
		std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0xffff'ffff'0000'0000ULL},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'ffffULL},
		std::uint64_t{0x0123'4567'89ab'cdefULL},
	};

	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation < 4; ++operation)
			for (unsigned encoding_index = 0;
			     encoding_index < encodings.size(); ++encoding_index)
				for (unsigned value_index = 0;
				     value_index < values.size(); ++value_index) {
					const auto &encoding =
						encodings[encoding_index];
					const auto instruction = encode_immediate(
						wide, operation, encoding.n,
						encoding.rotate, encoding.set_bits,
						13, 27);
					auto initial = seeded_state(
						0x3000'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(
							encoding_index) << 48) ^
						(static_cast<std::uint64_t>(
							value_index) << 40) ^
						instruction);
					initial.registers[13] = values[value_index];
					initial.nzcv = 0xf000'0000U;
					char name[144];
					std::snprintf(name, sizeof(name),
						"logical immediate boundary width=%u "
						"op=%u encoding=%u value=%u",
						wide ? 64U : 32U, operation,
						encoding_index, value_index);
					check(harness, name, Form::immediate,
						instruction, initial);
				}
}

void shifted_boundaries(EngineHarness &harness)
{
	constexpr std::array pairs{
		std::array{std::uint64_t{0}, std::uint64_t{0}},
		std::array{std::uint64_t{0}, std::uint64_t{1}},
		std::array{std::uint64_t{1}, std::uint64_t{1}},
		std::array{std::uint64_t{0x7fff'ffffU},
			std::uint64_t{0x8000'0000U}},
		std::array{std::uint64_t{0x8000'0000U},
			std::uint64_t{0xffff'ffffU}},
		std::array{std::uint64_t{0xffff'ffff'0000'0000ULL},
			std::uint64_t{0x8000'0000'0000'0001ULL}},
		std::array{std::uint64_t{0x7fff'ffff'ffff'ffffULL},
			std::uint64_t{0xffff'ffff'ffff'ffffULL}},
		std::array{std::uint64_t{0x0123'4567'89ab'cdefULL},
			std::uint64_t{0xfedc'ba98'7654'3210ULL}},
	};

	for (const bool wide : {false, true}) {
		const std::array amounts{
			0U, 1U, wide ? 31U : 15U, wide ? 63U : 31U};
		for (unsigned operation = 0; operation < 4; ++operation)
			for (const bool invert : {false, true})
				for (unsigned shift_type = 0; shift_type < 4;
				     ++shift_type)
					for (const auto amount : amounts)
						for (unsigned pair_index = 0;
						     pair_index < pairs.size();
						     ++pair_index) {
							const auto instruction = encode_shifted(
								wide, operation, invert,
								shift_type, amount,
								13, 27, 19);
							auto initial = seeded_state(
								0x3100'0000'0000'0000ULL ^
								(static_cast<std::uint64_t>(
									pair_index) << 48) ^
								instruction);
							initial.registers[13] =
								pairs[pair_index][0];
							initial.registers[27] =
								pairs[pair_index][1];
							initial.nzcv = 0xf000'0000U;
							char name[160];
							std::snprintf(name, sizeof(name),
								"logical shifted boundary "
								"width=%u op=%u N=%u type=%u "
								"amount=%u pair=%u",
								wide ? 64U : 32U,
								operation,
								invert ? 1U : 0U,
								shift_type, amount,
								pair_index);
							check(harness, name,
								Form::shifted_register,
								instruction, initial);
						}
	}
}

void every_tst_immediate_alias(EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (unsigned n = 0; n < 2; ++n)
			for (unsigned rotate = 0; rotate < 64; ++rotate)
				for (unsigned set_bits = 0; set_bits < 64;
				     ++set_bits) {
					const unsigned source =
						(rotate * 3U + set_bits) & 31U;
					const auto instruction = encode_immediate(
						wide, 3, n, rotate, set_bits,
						source, 31);
					if (!is_valid(Form::immediate, instruction))
						continue;
					auto initial = seeded_state(
						0x3200'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(
							instruction) << 23));
					initial.nzcv = 0xf000'0000U;
					char name[136];
					std::snprintf(name, sizeof(name),
						"TST immediate width=%u N=%u "
						"immr=%u imms=%u rn=%u",
						wide ? 64U : 32U, n, rotate,
						set_bits, source);
					check(harness, name, Form::immediate,
						instruction, initial);
				}
}

void every_mov_bitmask_alias(EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (unsigned n = 0; n < 2; ++n)
			for (unsigned rotate = 0; rotate < 64; ++rotate)
				for (unsigned set_bits = 0; set_bits < 64;
				     ++set_bits) {
					const unsigned destination =
						(rotate * 11U + set_bits) & 31U;
					const auto instruction = encode_immediate(
						wide, 1, n, rotate, set_bits,
						31, destination);
					if (!is_valid(Form::immediate, instruction) ||
					    move_wide_preferred(
						    wide, n, set_bits, rotate))
						continue;
					auto initial = seeded_state(
						0x3300'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(
							instruction) << 29));
					char name[144];
					std::snprintf(name, sizeof(name),
						"MOV bitmask width=%u N=%u immr=%u "
						"imms=%u rd=%u",
						wide ? 64U : 32U, n, rotate,
						set_bits, destination);
					check(harness, name, Form::immediate,
						instruction, initial);
				}
}

void every_mov_register_alias(EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (unsigned source = 0; source < 32; ++source)
			for (unsigned destination = 0; destination < 32;
			     ++destination) {
				const auto instruction = encode_shifted(
					wide, 1, false, 0, 0, 31,
					source, destination);
				auto initial = seeded_state(
					0x3400'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(source) << 46) ^
					(static_cast<std::uint64_t>(
						destination) << 38));
				char name[112];
				std::snprintf(name, sizeof(name),
					"MOV register width=%u rm=%u rd=%u",
					wide ? 64U : 32U, source, destination);
				check(harness, name, Form::shifted_register,
					instruction, initial);
			}
}

void every_mvn_alias(EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (unsigned shift_type = 0; shift_type < 4; ++shift_type)
			for (unsigned amount = 0;
			     amount < (wide ? 64U : 32U); ++amount) {
				const unsigned source =
					(amount * 5U + shift_type) & 31U;
				const unsigned destination =
					(amount * 13U + shift_type * 7U) & 31U;
				const auto instruction = encode_shifted(
					wide, 1, true, shift_type, amount,
					31, source, destination);
				auto initial = seeded_state(
					0x3500'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(
						instruction) << 31));
				char name[128];
				std::snprintf(name, sizeof(name),
					"MVN width=%u type=%u amount=%u "
					"rm=%u rd=%u",
					wide ? 64U : 32U, shift_type,
					amount, source, destination);
				check(harness, name, Form::shifted_register,
					instruction, initial);
			}
}

void every_tst_shifted_alias(EngineHarness &harness)
{
	constexpr std::array sources{
		std::array{1U, 2U},
		std::array{31U, 2U},
		std::array{1U, 31U},
		std::array{31U, 31U},
	};
	for (const bool wide : {false, true})
		for (unsigned shift_type = 0; shift_type < 4; ++shift_type)
			for (unsigned amount = 0;
			     amount < (wide ? 64U : 32U); ++amount)
				for (const auto &source : sources) {
					const auto instruction = encode_shifted(
						wide, 3, false, shift_type,
						amount, source[0], source[1], 31);
					auto initial = seeded_state(
						0x3600'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(
							instruction) << 33));
					initial.nzcv = 0xf000'0000U;
					char name[136];
					std::snprintf(name, sizeof(name),
						"TST shifted width=%u type=%u "
						"amount=%u rn=%u rm=%u",
						wide ? 64U : 32U, shift_type,
						amount, source[0], source[1]);
					check(harness, name,
						Form::shifted_register,
						instruction, initial);
				}
}

} // namespace

void run_behavior_tests(EngineHarness &harness)
{
	/*
	 * Direct operations cover BASE-0014/0015/0016/0017, BASE-0041/0042,
	 * BASE-0154/0155/0156, and BASE-0305/0306/0307.  The named sweeps cover
	 * BASE-0283, BASE-0285, BASE-0299, and BASE-0492/0493.
	 */
	immediate_boundaries(harness);
	shifted_boundaries(harness);
	every_tst_immediate_alias(harness);
	every_mov_bitmask_alias(harness);
	every_mov_register_alias(harness);
	every_mvn_alias(harness);
	every_tst_shifted_alias(harness);
}

} // namespace uml::backend::tests::base_logical
