// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_condition_test_support.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace test = uml::backend::tests::base_condition;
namespace engine_tests = uml::backend::tests;

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
	const uml::backend::arm64::CpuState &initial)
{
	test::check_compare(harness, name, instruction, initial);
	++cases;
}

void encoding_examples()
{
	require(test::encode_compare(false, false, false, 0, 0, 1, 2) ==
			0x3a42'0020U,
		"CCMN register W encoding mismatch");
	require(test::encode_compare(true, true, false, 13, 15, 31, 30) ==
			0xfa5e'd3efU,
		"CCMP register X encoding mismatch");
}

void every_condition_and_nzcv(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (unsigned condition = 0; condition < 16U; ++condition)
				for (unsigned incoming = 0; incoming < 16U;
				     ++incoming)
					for (unsigned alternative = 0;
					     alternative < 16U;
					     ++alternative) {
						auto initial = test::state(
							0x4400'0000'0000'0000ULL ^
							(static_cast<std::uint64_t>(
								 condition)
							 << 44U) ^
							(static_cast<std::uint64_t>(
								 incoming)
							 << 36U) ^
							(static_cast<std::uint64_t>(
								 alternative)
							 << 28U));
						initial.nzcv = incoming << 28U;
						initial.registers[3] =
							0x8000'0000'ffff'ffffULL;
						initial.registers[5] =
							0x7fff'ffff'0000'0001ULL;
						char name[192];
						const auto id = test::compare_id(
							subtract, false);
						std::snprintf(name,
							sizeof(name),
							"%.*s width=%u cond=%u incoming=%u alternative=%u",
							static_cast<int>(
								id.size()),
							id.data(),
							wide ? 64U : 32U,
							condition, incoming,
							alternative);
						check(harness, name,
							test::encode_compare(
								wide, subtract,
								false, condition,
								alternative, 3, 5),
							initial);
					}
}

void every_register_field_pair(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (unsigned rn = 0; rn < 32U; ++rn)
				for (unsigned rm = 0; rm < 32U; ++rm) {
					auto initial = test::state(
						0x5500'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(rn)
						 << 16U) ^
						rm);
					initial.nzcv = 0x4000'0000U;
					char name[176];
					const auto id =
						test::compare_id(subtract, false);
					std::snprintf(name, sizeof(name),
						"%.*s fields width=%u rn=%u rm=%u",
						static_cast<int>(id.size()),
						id.data(),
						wide ? 64U : 32U, rn, rm);
					check(harness, name,
						test::encode_compare(wide,
							subtract, false, 0, 9,
							rn, rm),
						initial);
				}
}

void arithmetic_boundaries_and_aliases(
	engine_tests::EngineHarness &harness)
{
	const std::array values{
		std::uint64_t{0},
		std::uint64_t{1},
		std::uint64_t{2},
		std::uint64_t{0x7fff'ffffU},
		std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'fffeULL},
		~std::uint64_t{0},
	};
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const auto left : values)
				for (const auto right : values) {
					auto initial = test::state(
						0x6600'0000'0000'0000ULL ^
						left ^ (right << 1U));
					initial.nzcv = 0x4000'0000U;
					initial.registers[3] = left;
					initial.registers[5] = right;
					char name[208];
					const auto id =
						test::compare_id(subtract, false);
					std::snprintf(name, sizeof(name),
						"%.*s boundary width=%u left=%#llx right=%#llx",
						static_cast<int>(id.size()),
						id.data(),
						wide ? 64U : 32U,
						static_cast<unsigned long long>(
							left),
						static_cast<unsigned long long>(
							right));
					check(harness, name,
						test::encode_compare(wide,
							subtract, false, 0, 0,
							3, 5),
						initial);
				}

	/* Rn == Rm must snapshot the single source before updating NZCV. */
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (unsigned source = 0; source < 32U; ++source)
				for (unsigned flags = 0; flags < 16U; ++flags) {
					auto initial = test::state(
						0x7700'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(
							 source)
						 << 8U) ^
						flags);
					initial.nzcv = flags << 28U;
					char name[176];
					const auto id =
						test::compare_id(subtract, false);
					std::snprintf(name, sizeof(name),
						"%.*s equal-source width=%u reg=%u nzcv=%u",
						static_cast<int>(id.size()),
						id.data(),
						wide ? 64U : 32U, source,
						flags);
					check(harness, name,
						test::encode_compare(wide,
							subtract, false, 14, 0,
							source, source),
						initial);
				}
}

} // namespace

int main()
{
	encoding_examples();
	engine_tests::EngineHarness harness;
	every_condition_and_nzcv(harness);
	every_register_field_pair(harness);
	arithmetic_boundaries_and_aliases(harness);
	const auto &counts = harness.counts();
	require(counts.interpreter == cases && counts.gadgets == cases,
		"register compare suite did not execute every case in both software engines");
	require(counts.jit + counts.jit_skipped == cases,
		"register compare suite did not execute or explicitly skip every JIT case");
	harness.print_summary("arm64-base-condition-compare-register");
	return 0;
}
