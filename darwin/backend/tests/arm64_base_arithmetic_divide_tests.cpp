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
	require(test::encode_divide(false, false, 0, 1, 2) ==
			0x1ac2'0820U,
		"UDIV W encoding mismatch");
	require(test::encode_divide(true, true, 7, 8, 9) ==
			0x9ac9'0d07U,
		"SDIV X encoding mismatch");
	require(test::encode_divide(true, false, 31, 31, 31) ==
			0x9adf'0bffU,
		"UDIV X zero/discard encoding mismatch");
}

void every_register_field(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const bool signed_divide : {false, true}) {
			for (unsigned destination = 0; destination < 32U;
			     ++destination) {
				auto initial = test::seeded_state(
					0x4100'0000ULL | destination);
				char name[128];
				std::snprintf(name, sizeof(name),
					"base-divide-rd width=%u signed=%u rd=%u",
					wide ? 64U : 32U,
					signed_divide ? 1U : 0U,
					destination);
				test::check_divide(harness, name,
					test::encode_divide(wide,
						signed_divide,
						destination, 3, 5),
					initial);
			}
			for (unsigned dividend = 0; dividend < 32U; ++dividend) {
				auto initial = test::seeded_state(
					0x4200'0000ULL | dividend);
				initial.registers[5] = 7U;
				char name[128];
				std::snprintf(name, sizeof(name),
					"base-divide-rn width=%u signed=%u rn=%u",
					wide ? 64U : 32U,
					signed_divide ? 1U : 0U,
					dividend);
				test::check_divide(harness, name,
					test::encode_divide(wide,
						signed_divide, 7,
						dividend, 5),
					initial);
			}
			for (unsigned divisor = 0; divisor < 32U; ++divisor) {
				auto initial = test::seeded_state(
					0x4300'0000ULL | divisor);
				if (divisor != 31U)
					initial.registers[divisor] =
						divisor + 1U;
				char name[128];
				std::snprintf(name, sizeof(name),
					"base-divide-rm width=%u signed=%u rm=%u",
					wide ? 64U : 32U,
					signed_divide ? 1U : 0U,
					divisor);
				test::check_divide(harness, name,
					test::encode_divide(wide,
						signed_divide, 7, 3,
						divisor),
					initial);
			}
		}
}

void boundary_division(engine_tests::EngineHarness &harness)
{
	const std::array values{
		std::uint64_t{0},
		std::uint64_t{1},
		std::uint64_t{2},
		std::uint64_t{3},
		std::uint64_t{0x7fff'ffffU},
		std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'fffdULL},
		std::uint64_t{0xffff'ffff'ffff'fffeULL},
		~std::uint64_t{0},
	};
	for (const bool wide : {false, true})
		for (const bool signed_divide : {false, true})
			for (const auto dividend : values)
				for (const auto divisor : values) {
					auto initial = test::seeded_state(
						dividend ^ (divisor << 1U));
					initial.registers[3] =
						wide ? dividend :
							dividend |
							0x5a5a'a5a5'0000'0000ULL;
					initial.registers[5] =
						wide ? divisor :
							divisor |
							0xa5a5'5a5a'0000'0000ULL;
					char name[160];
					std::snprintf(name, sizeof(name),
						"base-divide-boundary width=%u signed=%u dividend=%#llx divisor=%#llx",
						wide ? 64U : 32U,
						signed_divide ? 1U : 0U,
						static_cast<unsigned long long>(
							dividend),
						static_cast<unsigned long long>(
							divisor));
					test::check_divide(harness, name,
						test::encode_divide(wide,
							signed_divide, 7,
							3, 5),
						initial);
				}
}

void aliases_and_destructive_forms(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const bool signed_divide : {false, true})
			for (unsigned reg = 0; reg < 31U; ++reg) {
				const std::array<std::array<unsigned, 3>, 4> forms{{
					{reg, reg, 29U},
					{reg, 28U, reg},
					{27U, reg, reg},
					{reg, reg, reg},
				}};
				for (unsigned form = 0; form < forms.size(); ++form) {
					auto initial = test::seeded_state(
						0x4400'0000ULL ^
						(static_cast<std::uint64_t>(reg)
						 << 8U) ^
						form);
					const auto fields = forms[form];
					if (fields[2] != 31U &&
					    initial.registers[fields[2]] == 0U)
						initial.registers[fields[2]] = 3U;
					char name[144];
					std::snprintf(name, sizeof(name),
						"base-divide-alias width=%u signed=%u form=%u reg=%u",
						wide ? 64U : 32U,
						signed_divide ? 1U : 0U,
						form, reg);
					test::check_divide(harness, name,
						test::encode_divide(wide,
							signed_divide,
							fields[0],
							fields[1],
							fields[2]),
						initial);
				}
			}
}

void zero_register_and_flags(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const bool signed_divide : {false, true})
			for (unsigned flags = 0; flags < 16U; ++flags)
				for (const unsigned dividend : {3U, 31U})
					for (const unsigned divisor : {5U, 31U})
						for (const unsigned destination :
						     {7U, 31U}) {
							auto initial =
								test::seeded_state(
									0x4500'0000ULL |
									flags);
							initial.nzcv =
								flags << 28U;
							initial.registers[3] =
								0x8000'0000'0000'0015ULL;
							initial.registers[5] = 3U;
							test::check_divide(
								harness,
								"base-divide-zero-register-and-flags",
								test::encode_divide(
									wide,
									signed_divide,
									destination,
									dividend,
									divisor),
								initial);
						}
}

} // namespace

int main()
{
	encoding_examples();
	engine_tests::EngineHarness harness;
	every_register_field(harness);
	boundary_division(harness);
	aliases_and_destructive_forms(harness);
	zero_register_and_flags(harness);
	harness.print_summary("arm64-base-divide");
	return 0;
}
