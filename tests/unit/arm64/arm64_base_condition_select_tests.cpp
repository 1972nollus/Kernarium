// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_condition_test_support.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace test = uml::emulator::tests::base_condition;
namespace engine_tests = uml::emulator::tests;

namespace {

std::size_t cases{};

[[noreturn]] void fail(const char *message)
{
	std::fprintf(stderr, "%s\n", message);
	std::abort();
}

void require(bool condition, const char *message)
{
	if (!condition)
		fail(message);
}

void check(engine_tests::EngineHarness &harness, const char *name,
	std::uint32_t instruction,
	const uml::cpu::arm64::CpuState &initial)
{
	test::check_select(harness, name, instruction, initial);
	++cases;
}

void encoding_examples()
{
	require(test::encode_select(false, test::SelectOperation::select, 0,
			0, 1, 2) == 0x1a82'0020U,
		"CSEL W encoding mismatch");
	require(test::encode_select(true, test::SelectOperation::increment, 1,
			7, 8, 9) == 0x9a89'1507U,
		"CSINC X encoding mismatch");
	require(test::encode_select(false, test::SelectOperation::invert, 10,
			13, 5, 31) == 0x5a9f'a0adU,
		"CSINV W encoding mismatch");
	require(test::encode_select(true, test::SelectOperation::negate, 13,
			31, 31, 31) == 0xda9f'd7ffU,
		"CSNEG X discard encoding mismatch");
	require(test::encode_select_alias(true,
			test::SelectOperation::increment, 0, 7, 5) ==
			0x9a85'14a7U,
		"CINC X alias encoding mismatch");
	require(test::encode_set_alias(false,
			test::SelectOperation::invert, 1, 9) ==
			0x5a9f'03e9U,
		"CSETM W alias encoding mismatch");
}

void every_condition_and_flag_state(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const auto description : test::select_descriptions)
			for (unsigned condition = 0; condition < 16U; ++condition)
				for (unsigned flags = 0; flags < 16U; ++flags) {
					auto initial = test::state(
						0x1000'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(wide)
						 << 52U) ^
						(static_cast<std::uint64_t>(
							 static_cast<unsigned>(
								 description.operation))
						 << 48U) ^
						(static_cast<std::uint64_t>(
							 condition)
						 << 40U) ^
						(static_cast<std::uint64_t>(flags)
						 << 32U));
					initial.nzcv = flags << 28U;
					initial.registers[3] =
						0x8000'0000'ffff'ffffULL;
					initial.registers[5] =
						0x7fff'ffff'0000'0001ULL;
					char name[160];
					std::snprintf(name, sizeof(name),
						"%.*s width=%u cond=%u nzcv=%u",
						static_cast<int>(
							description.id.size()),
						description.id.data(),
						wide ? 64U : 32U,
						condition, flags);
					check(harness, name,
						test::encode_select(wide,
							description.operation,
							condition, 7, 3, 5),
						initial);
				}
}

void every_register_field(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const auto description : test::select_descriptions) {
			for (unsigned rd = 0; rd < 32U; ++rd) {
				auto initial = test::state(
					0x2000'0000'0000'0000ULL ^ rd);
				initial.nzcv = 0x4000'0000U;
				char name[160];
				std::snprintf(name, sizeof(name),
					"%.*s destination-field width=%u rd=%u",
					static_cast<int>(description.id.size()),
					description.id.data(),
					wide ? 64U : 32U, rd);
				check(harness, name,
					test::encode_select(wide,
						description.operation, 0, rd,
						3, 5),
					initial);
			}
			for (unsigned rn = 0; rn < 32U; ++rn) {
				auto initial = test::state(
					0x3000'0000'0000'0000ULL ^ rn);
				initial.nzcv = 0x4000'0000U;
				char name[160];
				std::snprintf(name, sizeof(name),
					"%.*s first-source-field width=%u rn=%u",
					static_cast<int>(description.id.size()),
					description.id.data(),
					wide ? 64U : 32U, rn);
				check(harness, name,
					test::encode_select(wide,
						description.operation, 0, 7,
						rn, 5),
					initial);
			}
			for (unsigned rm = 0; rm < 32U; ++rm) {
				auto initial = test::state(
					0x4000'0000'0000'0000ULL ^ rm);
				initial.nzcv = 0;
				char name[160];
				std::snprintf(name, sizeof(name),
					"%.*s second-source-field width=%u rm=%u",
					static_cast<int>(description.id.size()),
					description.id.data(),
					wide ? 64U : 32U, rm);
				check(harness, name,
					test::encode_select(wide,
						description.operation, 0, 7,
						3, rm),
					initial);
			}
		}
}

void zero_and_discard_combinations(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const auto description : test::select_descriptions)
			for (const std::uint32_t flags :
			     {0U, 0x4000'0000U})
				for (unsigned combination = 0;
				     combination < 8U; ++combination) {
					const unsigned rd =
						(combination & 4U) != 0 ? 31U : 7U;
					const unsigned rn =
						(combination & 2U) != 0 ? 31U : 3U;
					const unsigned rm =
						(combination & 1U) != 0 ? 31U : 5U;
					auto initial = test::state(
						0x5000'0000'0000'0000ULL ^
						combination ^
						static_cast<std::uint64_t>(flags));
					initial.nzcv = flags;
					char name[176];
					std::snprintf(name, sizeof(name),
						"%.*s zero-discard width=%u flags=%u rd=%u rn=%u rm=%u",
						static_cast<int>(
							description.id.size()),
						description.id.data(),
						wide ? 64U : 32U,
						flags >> 28U, rd, rn, rm);
					check(harness, name,
						test::encode_select(wide,
							description.operation, 0,
							rd, rn, rm),
						initial);
				}
}

void destructive_aliases(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const auto description : test::select_descriptions)
			for (const unsigned condition : {0U, 1U})
				for (unsigned reg = 0; reg < 31U; ++reg) {
					const std::array<std::array<unsigned, 3>, 4>
						fields{{
							{reg, reg, 29U},
							{reg, 28U, reg},
							{27U, reg, reg},
							{reg, reg, reg},
						}};
					for (unsigned form = 0;
					     form < fields.size(); ++form) {
						auto initial = test::state(
							0x6000'0000'0000'0000ULL ^
							(static_cast<std::uint64_t>(
								 reg)
							 << 8U) ^
							form);
						initial.nzcv =
							condition == 0U ?
								0x4000'0000U :
								0U;
						const auto [rd, rn, rm] =
							fields[form];
						char name[176];
						std::snprintf(name, sizeof(name),
							"%.*s destructive width=%u cond=%u reg=%u form=%u",
							static_cast<int>(
								description.id
									.size()),
							description.id.data(),
							wide ? 64U : 32U,
							condition, reg, form);
						check(harness, name,
							test::encode_select(
								wide,
								description
									.operation,
								condition,
								rd, rn, rm),
							initial);
					}
				}
}

void exact_alias_instructions(engine_tests::EngineHarness &harness)
{
	struct RegisterAlias final {
		test::SelectOperation operation;
		const char *id;
	};
	const std::array aliases{
		RegisterAlias{test::SelectOperation::increment,
			"BASE-0086 CINC"},
		RegisterAlias{test::SelectOperation::invert,
			"BASE-0087 CINV"},
		RegisterAlias{test::SelectOperation::negate,
			"BASE-0099 CNEG"},
	};
	for (const bool wide : {false, true})
		for (const auto alias : aliases)
			for (unsigned condition = 0; condition < 14U; ++condition)
				for (unsigned flags = 0; flags < 16U; ++flags) {
					auto initial = test::state(
						0x7000'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(
							 condition)
						 << 40U) ^
						(static_cast<std::uint64_t>(flags)
						 << 32U));
					initial.nzcv = flags << 28U;
					initial.registers[5] =
						0x8000'0000'ffff'ffffULL;
					char name[144];
					std::snprintf(name, sizeof(name),
						"%s width=%u cond=%u nzcv=%u",
						alias.id, wide ? 64U : 32U,
						condition, flags);
					check(harness, name,
						test::encode_select_alias(wide,
							alias.operation,
							condition, 7, 5),
						initial);
				}

	const std::array set_aliases{
		RegisterAlias{test::SelectOperation::increment,
			"BASE-0139 CSET"},
		RegisterAlias{test::SelectOperation::invert,
			"BASE-0140 CSETM"},
	};
	for (const bool wide : {false, true})
		for (const auto alias : set_aliases)
			for (unsigned condition = 0; condition < 14U; ++condition)
				for (unsigned flags = 0; flags < 16U; ++flags) {
					auto initial = test::state(
						0x8000'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(
							 condition)
						 << 40U) ^
						(static_cast<std::uint64_t>(flags)
						 << 32U));
					initial.nzcv = flags << 28U;
					char name[144];
					std::snprintf(name, sizeof(name),
						"%s width=%u cond=%u nzcv=%u",
						alias.id, wide ? 64U : 32U,
						condition, flags);
					check(harness, name,
						test::encode_set_alias(wide,
							alias.operation,
							condition, 7),
						initial);
				}
}

} // namespace

int main()
{
	encoding_examples();
	engine_tests::EngineHarness harness;
	every_condition_and_flag_state(harness);
	every_register_field(harness);
	zero_and_discard_combinations(harness);
	destructive_aliases(harness);
	exact_alias_instructions(harness);
	const auto &counts = harness.counts();
	require(counts.interpreter == cases && counts.gadgets == cases,
		"select suite did not execute every semantic case in both software engines");
	require(counts.jit + counts.jit_skipped == cases,
		"select suite did not execute or explicitly skip every JIT case");
	harness.print_summary("arm64-base-condition-select");
	return 0;
}
