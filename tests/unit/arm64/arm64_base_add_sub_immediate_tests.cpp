// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_add_sub_test_support.hpp"

#include <array>
#include <cstdio>

namespace uml::emulator::tests::base_add_sub {
namespace {

[[nodiscard]] constexpr std::string_view operation_name(
	bool subtract, bool set_flags) noexcept
{
	if (subtract)
		return set_flags ? "SUBS" : "SUB";
	return set_flags ? "ADDS" : "ADD";
}

void every_immediate(EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool set_flags : {false, true})
				for (const bool shift_12 : {false, true})
					for (unsigned immediate = 0; immediate < 4096;
					     ++immediate) {
						/*
						 * The independent low and middle five-bit groups
						 * enumerate all 32x32 source/destination pairs four
						 * times while the complete immediate field is swept.
						 */
						const unsigned source = immediate & 31U;
						const unsigned destination =
							(immediate >> 5) & 31U;
						const auto instruction = encode_immediate(
							wide, subtract, set_flags, shift_12,
							immediate, source, destination);
						auto initial = seeded_state(
							0x1000'0000'0000'0000ULL ^
							(static_cast<std::uint64_t>(instruction) << 13) ^
							immediate);
						char name[128];
						const auto operation =
							operation_name(subtract, set_flags);
						std::snprintf(name, sizeof(name),
							"%.*s immediate width=%u sh=%u imm=%u "
							"rn=%u rd=%u",
							static_cast<int>(operation.size()),
							operation.data(), wide ? 64U : 32U,
							shift_12 ? 12U : 0U, immediate,
							source, destination);
						check(harness, name, Form::immediate,
							instruction, initial);
					}
}

void independent_native_specimen(EngineHarness &harness)
{
	arm64::CpuState initial{};
	initial.registers[5] = 0x25;
	initial.nzcv = 0xa000'0000U;
	check(harness,
		"ADD immediate native specimen width=64 sh=0 imm=5 rn=5 rd=0",
		Form::immediate,
		encode_immediate(true, false, false, false, 5, 5, 0),
		initial);
}

void arithmetic_boundaries(EngineHarness &harness)
{
	const std::array left_values{
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
	const std::array immediates{0U, 1U, 0x7ffU, 0x800U, 0xfffU};

	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool set_flags : {false, true})
				for (const bool shift_12 : {false, true})
					for (const auto immediate : immediates)
						for (unsigned value_index = 0;
						     value_index < left_values.size();
						     ++value_index) {
							const auto instruction = encode_immediate(
								wide, subtract, set_flags, shift_12,
								immediate, 13, 27);
							auto initial = seeded_state(
								0x2000'0000'0000'0000ULL ^
								(static_cast<std::uint64_t>(
									value_index) << 48) ^
								instruction);
							initial.registers[13] =
								left_values[value_index];
							char name[128];
							const auto operation =
								operation_name(subtract, set_flags);
							std::snprintf(name, sizeof(name),
								"%.*s immediate boundary width=%u "
								"sh=%u imm=%u value=%u",
								static_cast<int>(operation.size()),
								operation.data(),
								wide ? 64U : 32U,
								shift_12 ? 12U : 0U,
								immediate, value_index);
							check(harness, name, Form::immediate,
								instruction, initial);
						}
}

void aliases_and_special_registers(EngineHarness &harness)
{
	/* MOV (to/from SP), including the WSP zero-extension behavior. */
	for (const bool wide : {false, true}) {
		for (unsigned reg = 0; reg < 32; ++reg) {
			auto initial = seeded_state(
				0x3000'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(wide) << 40) | reg);
			initial.sp = 0xffff'ffff'8000'0001ULL ^ reg;
			char name[96];
			std::snprintf(name, sizeof(name),
				"MOV from SP width=%u rd=%u",
				wide ? 64U : 32U, reg);
			check(harness, name, Form::immediate,
				encode_immediate(wide, false, false, false, 0,
					31, reg),
				initial);

			initial = seeded_state(
				0x3100'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(wide) << 40) | reg);
			if (reg != 31U)
				initial.registers[reg] =
					0xffff'ffff'8000'0001ULL ^ reg;
			std::snprintf(name, sizeof(name),
				"MOV to SP width=%u rn=%u",
				wide ? 64U : 32U, reg);
			check(harness, name, Form::immediate,
				encode_immediate(wide, false, false, false, 0,
					reg, 31),
				initial);
		}
	}

	/* CMN/CMP use SP as their source and discard the arithmetic result. */
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool shift_12 : {false, true})
				for (const unsigned immediate :
				     {0U, 1U, 0x800U, 0xfffU})
					for (const unsigned source : {0U, 30U, 31U}) {
						auto initial = seeded_state(
							0x4000'0000'0000'0000ULL ^
							(static_cast<std::uint64_t>(immediate) << 32) ^
							source);
						initial.sp = wide ?
							0x8000'0000'0000'0000ULL :
							0xffff'ffff'8000'0000ULL;
						char name[112];
						std::snprintf(name, sizeof(name),
							"%s immediate width=%u sh=%u imm=%u "
							"rn=%u",
							subtract ? "CMP" : "CMN",
							wide ? 64U : 32U,
							shift_12 ? 12U : 0U,
							immediate, source);
						check(harness, name, Form::immediate,
							encode_immediate(wide, subtract, true,
								shift_12, immediate, source, 31),
							initial);
					}

	/* Destructive Rd==Rn cases verify that sources are captured before write. */
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool set_flags : {false, true})
				for (const bool shift_12 : {false, true})
					for (const unsigned reg : {0U, 15U, 30U}) {
						auto initial = seeded_state(
							0x5000'0000'0000'0000ULL ^
							(static_cast<std::uint64_t>(reg) << 44));
						initial.registers[reg] =
							0x7fff'ffff'ffff'f001ULL;
						char name[112];
						std::snprintf(name, sizeof(name),
							"immediate destructive width=%u op=%u "
							"flags=%u sh=%u reg=%u",
							wide ? 64U : 32U,
							subtract ? 1U : 0U,
							set_flags ? 1U : 0U,
							shift_12 ? 1U : 0U, reg);
						check(harness, name, Form::immediate,
							encode_immediate(wide, subtract,
								set_flags, shift_12, 0xfffU,
								reg, reg),
							initial);
					}
}

} // namespace

void run_immediate_tests(EngineHarness &harness)
{
	every_immediate(harness);
	independent_native_specimen(harness);
	arithmetic_boundaries(harness);
	aliases_and_special_registers(harness);
}

} // namespace uml::emulator::tests::base_add_sub
