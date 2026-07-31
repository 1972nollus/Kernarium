// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_add_sub_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <span>

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
	Registers{31, 2, 7, "sp-left"},
	Registers{1, 31, 7, "zero-right"},
	Registers{1, 2, 31, "special-destination"},
	Registers{31, 31, 7, "sp-zero"},
	Registers{31, 2, 31, "sp-special-destination"},
	Registers{1, 31, 31, "zero-special-destination"},
};

void loaded_operand_controls_conditional_branch(EngineHarness &harness)
{
	std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> page{};
	constexpr auto loaded = engine_test_data_base + 0x1000U;
	for (unsigned byte = 0; byte < sizeof(loaded); ++byte)
		page[16U + byte] =
			static_cast<std::byte>(loaded >> (byte * 8U));
	const MemoryRegion memory{
		.address = engine_test_data_base,
		.initial = page,
		.expected = page,
	};
	constexpr std::array code{
		ProgramWord{0, 0xf940'0b90U}, // ldr x16, [x28, #16]
		ProgramWord{4, 0xeb30'63ffU}, // cmp sp, x16
		ProgramWord{8, 0x5400'0049U}, // b.ls +8
		ProgramWord{12, 0xd400'0001U},
		ProgramWord{16, 0xd400'0001U},
	};
	auto initial = seeded_state(0xd000'0000'0000'0000ULL);
	initial.registers[16] = static_cast<std::uint64_t>(-1314);
	initial.registers[28] = engine_test_data_base;
	initial.sp = engine_test_data_base + 0x2000U;
	initial.nzcv = 0;
	auto expected = initial;
	expected.registers[16] = loaded;
	expected.nzcv = 0x2000'0000U;
	expected.pc = engine_test_code_base + 16U;
	harness.check_program({
		.name = "loaded extended operand controls conditional branch",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = NativeRequirements::baseline(),
		.memory = std::span<const MemoryRegion>{&memory, 1},
	});
}

void every_extension_field(EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool set_flags : {false, true})
				for (unsigned extension = 0; extension < 8;
				     ++extension)
					for (unsigned amount = 0; amount <= 4;
					     ++amount)
						for (const auto &registers :
						     register_patterns) {
							const auto instruction = encode_extended(
								wide, subtract, set_flags,
								extension, amount,
								registers.left, registers.right,
								registers.destination);
							auto initial = seeded_state(
								0x8000'0000'0000'0000ULL ^
								(static_cast<std::uint64_t>(
									instruction) << 11) ^
								amount);
							char name[144];
							std::snprintf(name, sizeof(name),
								"extended width=%u op=%u flags=%u "
								"option=%u amount=%u %.*s",
								wide ? 64U : 32U,
								subtract ? 1U : 0U,
								set_flags ? 1U : 0U,
								extension, amount,
								static_cast<int>(
									registers.name.size()),
								registers.name.data());
							check(harness, name,
								Form::extended_register,
								instruction, initial);
						}
}

void extension_boundaries(EngineHarness &harness)
{
	const std::array values{
		std::uint64_t{0},
		std::uint64_t{1},
		std::uint64_t{0x7f},
		std::uint64_t{0x80},
		std::uint64_t{0xff},
		std::uint64_t{0x7fff},
		std::uint64_t{0x8000},
		std::uint64_t{0xffff},
		std::uint64_t{0x7fff'ffffU},
		std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'ffffULL},
	};
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool set_flags : {false, true})
				for (unsigned extension = 0; extension < 8;
				     ++extension)
					for (unsigned amount = 0; amount <= 4;
					     ++amount)
						for (unsigned value_index = 0;
						     value_index < values.size();
						     ++value_index) {
							auto initial = seeded_state(
								0x9000'0000'0000'0000ULL ^
								(static_cast<std::uint64_t>(
									value_index) << 44) ^
								(static_cast<std::uint64_t>(
									extension) << 36));
							initial.registers[13] =
								wide ?
								0x7fff'ffff'ffff'fff0ULL :
								0xffff'ffff'7fff'fff0ULL;
							initial.registers[27] =
								values[value_index];
							char name[144];
							std::snprintf(name, sizeof(name),
								"extended boundary width=%u op=%u "
								"flags=%u option=%u amount=%u "
								"value=%u",
								wide ? 64U : 32U,
								subtract ? 1U : 0U,
								set_flags ? 1U : 0U,
								extension, amount, value_index);
							check(harness, name,
								Form::extended_register,
								encode_extended(wide, subtract,
									set_flags, extension,
									amount, 13, 27, 19),
								initial);
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
							0xa000'0000'0000'0000ULL ^
							(static_cast<std::uint64_t>(reg) << 32));
						char name[112];
						std::snprintf(name, sizeof(name),
							"extended register sweep width=%u "
							"op=%u flags=%u reg=%u %.*s",
							wide ? 64U : 32U,
							subtract ? 1U : 0U,
							set_flags ? 1U : 0U, reg,
							static_cast<int>(
								registers.name.size()),
							registers.name.data());
						check(harness, name,
							Form::extended_register,
							encode_extended(wide, subtract,
								set_flags, reg & 7U,
								reg % 5U,
								registers.left,
								registers.right,
								registers.destination),
							initial);
					}
				}
}

void aliases(EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (unsigned extension = 0; extension < 8;
			     ++extension)
				for (const unsigned amount : {0U, 1U, 4U}) {
					auto initial = seeded_state(
						0xb000'0000'0000'0000ULL |
						(static_cast<std::uint64_t>(
							extension) << 40) |
						amount);
					initial.sp = wide ?
						0x8000'0000'0000'0010ULL :
						0xffff'ffff'8000'0010ULL;
					char name[104];
					std::snprintf(name, sizeof(name),
						"%s extended width=%u option=%u "
						"amount=%u SP-source",
						subtract ? "CMP" : "CMN",
						wide ? 64U : 32U,
						extension, amount);
					check(harness, name, Form::extended_register,
						encode_extended(wide, subtract, true,
							extension, amount, 31, 27, 31),
						initial);
				}
}

void reserved_encodings(EngineHarness &harness)
{
	/* imm3 values 5, 6, and 7 are reserved for every extension type. */
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool set_flags : {false, true})
				for (unsigned extension = 0; extension < 8;
				     ++extension)
					for (unsigned amount = 5; amount < 8;
					     ++amount)
						for (const auto registers :
						     {Registers{1, 2, 7, "ordinary"},
						      Registers{31, 31, 31, "special"},
						      Registers{7, 7, 7, "alias"}}) {
							const auto instruction = encode_extended(
								wide, subtract, set_flags,
								extension, amount,
								registers.left, registers.right,
								registers.destination);
							auto initial = seeded_state(
								0xc000'0000'0000'0000ULL ^
								instruction);
							char name[128];
							std::snprintf(name, sizeof(name),
								"reserved extended shift width=%u "
								"op=%u flags=%u option=%u "
								"amount=%u %.*s",
								wide ? 64U : 32U,
								subtract ? 1U : 0U,
								set_flags ? 1U : 0U,
								extension, amount,
								static_cast<int>(
									registers.name.size()),
								registers.name.data());
							check_illegal(harness, name,
								Form::extended_register,
								instruction, initial);
						}
}

} // namespace

void run_extended_tests(EngineHarness &harness)
{
	loaded_operand_controls_conditional_branch(harness);
	every_extension_field(harness);
	extension_boundaries(harness);
	every_register_number(harness);
	aliases(harness);
	reserved_encodings(harness);
}

} // namespace uml::backend::tests::base_add_sub
