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
	require(test::encode_compare(false, false, true, 0, 0, 1, 2) ==
			0x3a42'0820U,
		"CCMN immediate W encoding mismatch");
	require(test::encode_compare(true, true, true, 13, 15, 31, 31) ==
			0xfa5f'dbefU,
		"CCMP immediate X encoding mismatch");
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
							0x1100'0000'0000'0000ULL ^
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
						initial.registers[3] = wide ?
							0x7fff'ffff'ffff'fff0ULL :
							0xa5a5'5a5a'7fff'fff0ULL;
						const unsigned immediate =
							(condition * 7U +
							 incoming * 3U +
							 alternative) &
							31U;
						char name[192];
						const auto id = test::compare_id(
							subtract, true);
						std::snprintf(name,
							sizeof(name),
							"%.*s width=%u cond=%u incoming=%u alternative=%u imm=%u",
							static_cast<int>(
								id.size()),
							id.data(),
							wide ? 64U : 32U,
							condition, incoming,
							alternative,
							immediate);
						check(harness, name,
							test::encode_compare(
								wide, subtract,
								true, condition,
								alternative, 3,
								immediate),
							initial);
					}
}

void every_register_and_immediate_field(
	engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (unsigned rn = 0; rn < 32U; ++rn)
				for (unsigned immediate = 0; immediate < 32U;
				     ++immediate) {
					auto initial = test::state(
						0x2200'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(rn)
						 << 16U) ^
						immediate);
					/* EQ is true: every Rn and imm5 value is observed. */
					initial.nzcv = 0x4000'0000U;
					char name[176];
					const auto id =
						test::compare_id(subtract, true);
					std::snprintf(name, sizeof(name),
						"%.*s fields width=%u rn=%u imm=%u",
						static_cast<int>(id.size()),
						id.data(),
						wide ? 64U : 32U, rn,
						immediate);
					check(harness, name,
						test::encode_compare(wide,
							subtract, true, 0, 9,
							rn, immediate),
						initial);
				}
}

void arithmetic_boundaries(engine_tests::EngineHarness &harness)
{
	const std::array values{
		std::uint64_t{0},
		std::uint64_t{1},
		std::uint64_t{31},
		std::uint64_t{0x7fff'ffffU},
		std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'ffe1ULL},
		~std::uint64_t{0},
	};
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const auto left : values)
				for (unsigned immediate = 0; immediate < 32U;
				     ++immediate) {
					auto initial = test::state(
						0x3300'0000'0000'0000ULL ^
						left ^
						(static_cast<std::uint64_t>(
							 immediate)
						 << 48U));
					initial.nzcv = 0x4000'0000U;
					initial.registers[3] = left;
					char name[192];
					const auto id =
						test::compare_id(subtract, true);
					std::snprintf(name, sizeof(name),
						"%.*s boundary width=%u left=%#llx imm=%u",
						static_cast<int>(id.size()),
						id.data(),
						wide ? 64U : 32U,
						static_cast<unsigned long long>(
							left),
						immediate);
					check(harness, name,
						test::encode_compare(wide,
							subtract, true, 0, 0,
							3, immediate),
						initial);
				}
}

void unconditional_zero_source(engine_tests::EngineHarness &harness)
{
	/*
	 * AL/NV take the direct native conditional-compare path. Exercise every
	 * width, operation, condition, and imm5 value with architectural WZR/XZR.
	 * Keep the false-condition alternative nonzero so an implementation that
	 * accidentally consults incoming flags cannot match the arithmetic result
	 * merely because both happen to encode NZCV=0.
	 */
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const unsigned condition : {14U, 15U})
				for (unsigned immediate = 0; immediate < 32U;
				     ++immediate) {
					/*
					 * Zero plus imm5 produces NZCV 0 or 4;
					 * zero minus imm5 produces NZCV 6 or 8.
					 * Alternative 1 therefore differs from every
					 * expected arithmetic result in this matrix.
					 */
					constexpr unsigned alternative = 1U;
					auto initial = test::state(
						0x3f00'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(
							 condition)
						 << 40U) ^
						immediate);
					initial.nzcv =
						(immediate & 15U) << 28U;
					initial.sp =
						0x1234'5678'9abc'def0ULL;
					char name[192];
					const auto id =
						test::compare_id(subtract, true);
					std::snprintf(name, sizeof(name),
						"%.*s unconditional-zr width=%u cond=%u imm=%u",
						static_cast<int>(id.size()),
						id.data(),
						wide ? 64U : 32U,
						condition, immediate);
					check(harness, name,
						test::encode_compare(wide,
							subtract, true,
							condition,
							alternative, 31,
							immediate),
						initial);
				}
}

} // namespace

int main()
{
	encoding_examples();
	engine_tests::EngineHarness harness;
	every_condition_and_nzcv(harness);
	every_register_and_immediate_field(harness);
	arithmetic_boundaries(harness);
	unconditional_zero_source(harness);
	const auto &counts = harness.counts();
	require(counts.interpreter == cases && counts.gadgets == cases,
		"immediate compare suite did not execute every case in both software engines");
	require(counts.jit + counts.jit_skipped == cases,
		"immediate compare suite did not execute or explicitly skip every JIT case");
	harness.print_summary("arm64-base-condition-compare-immediate");
	return 0;
}
