// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_arithmetic_test_support.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace test = uml::backend::tests::base_arithmetic;
namespace engine_tests = uml::backend::tests;

namespace {

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

void encoding_examples()
{
	require(test::encode_carry(false, false, false, 0, 1, 2) ==
			0x1a02'0020U,
		"ADC W encoding mismatch");
	require(test::encode_carry(true, false, true, 7, 8, 9) ==
			0xba09'0107U,
		"ADCS X encoding mismatch");
	require(test::encode_carry(false, true, false, 13, 31, 5) ==
			0x5a05'03edU,
		"NGC W alias encoding mismatch");
	require(test::encode_carry(true, true, true, 31, 31, 31) ==
			0xfa1f'03ffU,
		"NGCS X discard encoding mismatch");
}

void every_register_field(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool set_flags : {false, true}) {
				for (unsigned destination = 0; destination < 32U;
				     ++destination) {
					auto initial = test::seeded_state(
						0x1000'0000ULL |
						(static_cast<std::uint64_t>(
							 wide)
						 << 44U) |
						(static_cast<std::uint64_t>(
							 subtract)
						 << 40U) |
						(static_cast<std::uint64_t>(
							 set_flags)
						 << 36U) |
						destination);
					char name[128];
					std::snprintf(name, sizeof(name),
						"base-carry-rd width=%u sub=%u flags=%u rd=%u",
						wide ? 64U : 32U,
						subtract ? 1U : 0U,
						set_flags ? 1U : 0U,
						destination);
					test::check_carry(harness, name,
						test::encode_carry(wide,
							subtract,
							set_flags,
							destination, 3, 5),
						initial);
				}
				for (unsigned left = 0; left < 32U; ++left) {
					auto initial = test::seeded_state(
						0x2000'0000ULL |
						(static_cast<std::uint64_t>(
							 wide)
						 << 44U) |
						(static_cast<std::uint64_t>(
							 subtract)
						 << 40U) |
						(static_cast<std::uint64_t>(
							 set_flags)
						 << 36U) |
						left);
					char name[128];
					std::snprintf(name, sizeof(name),
						"base-carry-rn width=%u sub=%u flags=%u rn=%u",
						wide ? 64U : 32U,
						subtract ? 1U : 0U,
						set_flags ? 1U : 0U, left);
					test::check_carry(harness, name,
						test::encode_carry(wide,
							subtract,
							set_flags, 7,
							left, 5),
						initial);
				}
				for (unsigned right = 0; right < 32U; ++right) {
					auto initial = test::seeded_state(
						0x3000'0000ULL |
						(static_cast<std::uint64_t>(
							 wide)
						 << 44U) |
						(static_cast<std::uint64_t>(
							 subtract)
						 << 40U) |
						(static_cast<std::uint64_t>(
							 set_flags)
						 << 36U) |
						right);
					char name[128];
					std::snprintf(name, sizeof(name),
						"base-carry-rm width=%u sub=%u flags=%u rm=%u",
						wide ? 64U : 32U,
						subtract ? 1U : 0U,
						set_flags ? 1U : 0U, right);
					test::check_carry(harness, name,
						test::encode_carry(wide,
							subtract,
							set_flags, 7, 3,
							right),
						initial);
				}
			}
}

void boundary_arithmetic(engine_tests::EngineHarness &harness)
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
			for (const bool set_flags : {false, true})
				for (const bool carry : {false, true})
					for (const auto left : values)
						for (const auto right : values) {
							auto initial =
								test::seeded_state(
									left ^
									(right << 1U));
							initial.registers[3] =
								wide ? left :
								left |
								0x5a5a'a5a5'0000'0000ULL;
							initial.registers[5] =
								wide ? right :
								right |
								0xa5a5'5a5a'0000'0000ULL;
							initial.nzcv =
								(carry ?
									 0x2000'0000U :
									 0U) |
								0x9000'0000U;
							char name[160];
							std::snprintf(name,
								sizeof(name),
								"base-carry-boundary width=%u sub=%u flags=%u carry=%u left=%#llx right=%#llx",
								wide ? 64U :
									32U,
								subtract ? 1U :
									0U,
								set_flags ? 1U :
									0U,
								carry ? 1U : 0U,
								static_cast<unsigned long long>(
									left),
								static_cast<unsigned long long>(
									right));
							test::check_carry(
								harness, name,
								test::encode_carry(
									wide,
									subtract,
									set_flags,
									7, 3, 5),
								initial);
						}
}

void aliases_and_destructive_forms(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool set_flags : {false, true})
				for (const bool carry : {false, true})
					for (unsigned reg = 0; reg < 31U; ++reg) {
						const std::array<std::array<unsigned, 3>, 4>
							forms{{
								{reg, reg, 29U},
								{reg, 28U, reg},
								{27U, reg, reg},
								{reg, reg, reg},
							}};
						for (unsigned form = 0;
						     form < forms.size(); ++form) {
							auto initial =
								test::seeded_state(
									0xabc0'0000ULL ^
									(static_cast<std::uint64_t>(
										 reg)
									 << 8U) ^
									form);
							initial.nzcv =
								(carry ?
									 0x2000'0000U :
									 0U) |
								0x9000'0000U;
							const auto fields =
								forms[form];
							char name[160];
							std::snprintf(name,
								sizeof(name),
								"base-carry-alias width=%u sub=%u flags=%u carry=%u form=%u reg=%u",
								wide ? 64U :
									32U,
								subtract ? 1U :
									0U,
								set_flags ? 1U :
									0U,
								carry ? 1U : 0U,
								form, reg);
							test::check_carry(
								harness, name,
								test::encode_carry(
									wide,
									subtract,
									set_flags,
									fields[0],
									fields[1],
									fields[2]),
								initial);
						}
					}
}

void negate_aliases(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const bool set_flags : {false, true})
			for (const bool carry : {false, true})
				for (unsigned source = 0; source < 32U; ++source)
					for (const unsigned destination : {7U, 31U}) {
						auto initial = test::seeded_state(
							0x7654'3210ULL ^
							(static_cast<std::uint64_t>(
								 source)
							 << 12U));
						initial.nzcv =
							(carry ? 0x2000'0000U : 0U) |
							0x9000'0000U;
						char name[144];
						std::snprintf(name, sizeof(name),
							"base-%s width=%u carry=%u rm=%u rd=%u",
							set_flags ? "ngcs" :
								"ngc",
							wide ? 64U : 32U,
							carry ? 1U : 0U,
							source, destination);
						test::check_carry(harness, name,
							test::encode_carry(
								wide, true,
								set_flags,
								destination,
								31, source),
							initial);
					}
}

void preserved_flags(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (unsigned flags = 0; flags < 16U; ++flags) {
				auto initial = test::seeded_state(
					0x55aa'0000ULL | flags);
				initial.nzcv = flags << 28U;
				test::check_carry(harness,
					"base-carry-preserves-nzcv",
					test::encode_carry(wide, subtract,
						false, 7, 3, 5),
					initial);
			}
}

} // namespace

int main()
{
	encoding_examples();
	engine_tests::EngineHarness harness;
	every_register_field(harness);
	boundary_arithmetic(harness);
	aliases_and_destructive_forms(harness);
	negate_aliases(harness);
	preserved_flags(harness);
	harness.print_summary("arm64-base-carry");
	return 0;
}
