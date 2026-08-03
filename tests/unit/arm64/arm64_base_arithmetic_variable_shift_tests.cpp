// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_arithmetic_test_support.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace test = uml::emulator::tests::base_arithmetic;
namespace engine_tests = uml::emulator::tests;

namespace {

constexpr std::array shift_names{
	"BASE-0276 LSLV", "BASE-0279 LSRV",
	"BASE-0021 ASRV", "BASE-0354 RORV",
};

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
	require(test::encode_variable_shift(false, test::ShiftOperation::left,
			0, 1, 2) == 0x1ac2'2020U,
		"LSLV W encoding mismatch");
	require(test::encode_variable_shift(true, test::ShiftOperation::right,
			7, 8, 9) == 0x9ac9'2507U,
		"LSRV X encoding mismatch");
	require(test::encode_variable_shift(false,
			test::ShiftOperation::arithmetic_right,
			13, 31, 5) == 0x1ac5'2bedU,
		"ASRV W zero-source encoding mismatch");
	require(test::encode_variable_shift(true,
			test::ShiftOperation::rotate_right,
			31, 31, 31) == 0x9adf'2fffU,
		"RORV X zero/discard encoding mismatch");
}

void every_register_field(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation < 4U; ++operation) {
			const auto shift =
				static_cast<test::ShiftOperation>(operation);
			for (unsigned destination = 0; destination < 32U;
			     ++destination) {
				auto initial = test::seeded_state(
					0x5100'0000ULL | destination);
				char name[136];
				std::snprintf(name, sizeof(name),
					"%s base-variable-shift-rd width=%u op=%u rd=%u",
					shift_names[operation],
					wide ? 64U : 32U, operation,
					destination);
				test::check_variable_shift(harness, name,
					test::encode_variable_shift(wide,
						shift, destination, 3, 5),
					initial);
			}
			for (unsigned value = 0; value < 32U; ++value) {
				auto initial = test::seeded_state(
					0x5200'0000ULL | value);
				initial.registers[5] = 13U;
				char name[136];
				std::snprintf(name, sizeof(name),
					"%s base-variable-shift-rn width=%u op=%u rn=%u",
					shift_names[operation],
					wide ? 64U : 32U, operation, value);
				test::check_variable_shift(harness, name,
					test::encode_variable_shift(wide,
						shift, 7, value, 5),
					initial);
			}
			for (unsigned amount = 0; amount < 32U; ++amount) {
				auto initial = test::seeded_state(
					0x5300'0000ULL | amount);
				if (amount != 31U)
					initial.registers[amount] =
						amount + 1U;
				char name[136];
				std::snprintf(name, sizeof(name),
					"%s base-variable-shift-rm width=%u op=%u rm=%u",
					shift_names[operation],
					wide ? 64U : 32U, operation,
					amount);
				test::check_variable_shift(harness, name,
					test::encode_variable_shift(wide,
						shift, 7, 3,
						amount),
					initial);
			}
		}
}

void every_effective_shift(engine_tests::EngineHarness &harness)
{
	const std::array values{
		std::uint64_t{0},
		std::uint64_t{1},
		std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x0123'4567'89ab'cdefULL},
		std::uint64_t{0x8000'0000'0000'0001ULL},
		~std::uint64_t{0},
	};
	for (const bool wide : {false, true}) {
		const unsigned bits = wide ? 64U : 32U;
		for (unsigned operation = 0; operation < 4U; ++operation)
			for (unsigned amount = 0; amount < bits; ++amount)
				for (const auto value : values) {
					auto initial = test::seeded_state(
						value ^
						(static_cast<std::uint64_t>(
							 amount)
						 << 32U));
					initial.registers[3] =
						wide ? value :
							value |
							0xa5a5'5a5a'0000'0000ULL;
					initial.registers[5] = amount;
					char name[160];
					std::snprintf(name, sizeof(name),
						"%s base-variable-shift-effective width=%u op=%u amount=%u value=%#llx",
						shift_names[operation],
						bits, operation, amount,
						static_cast<unsigned long long>(
							value));
					test::check_variable_shift(harness, name,
						test::encode_variable_shift(
							wide,
							static_cast<
								test::ShiftOperation>(
								operation),
							7, 3, 5),
						initial);
				}
	}
}

void shift_masking(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true}) {
		const unsigned bits = wide ? 64U : 32U;
		for (unsigned operation = 0; operation < 4U; ++operation)
			for (unsigned amount = 0; amount < bits; ++amount)
				for (const std::uint64_t high :
				     {std::uint64_t{0},
				      static_cast<std::uint64_t>(bits),
				      static_cast<std::uint64_t>(bits * 3U),
				      0xfedc'ba98'0000'0000ULL}) {
					auto initial = test::seeded_state(
						0x5400'0000ULL |
						(static_cast<std::uint64_t>(
							 operation)
						 << 16U) |
						amount);
					initial.registers[3] =
						0x89ab'cdef'8123'4567ULL;
					initial.registers[5] = high | amount;
					test::check_variable_shift(harness,
						shift_names[operation],
						test::encode_variable_shift(
							wide,
							static_cast<
								test::ShiftOperation>(
								operation),
							7, 3, 5),
						initial);
				}
	}
}

void aliases_and_preserved_state(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation < 4U; ++operation)
			for (unsigned reg = 0; reg < 31U; ++reg) {
				const std::array<std::array<unsigned, 3>, 4> forms{{
					{reg, reg, 29U},
					{reg, 28U, reg},
					{27U, reg, reg},
					{reg, reg, reg},
				}};
				for (unsigned form = 0;
				     form < forms.size(); ++form) {
					auto initial = test::seeded_state(
						0x5500'0000ULL ^
						(static_cast<std::uint64_t>(reg)
						 << 8U) ^
						form);
					initial.nzcv = 0xb000'0000U;
					const auto fields = forms[form];
					char name[160];
					std::snprintf(name, sizeof(name),
						"%s base-variable-shift-alias width=%u op=%u form=%u reg=%u",
						shift_names[operation],
						wide ? 64U : 32U,
						operation, form, reg);
					test::check_variable_shift(
						harness, name,
						test::encode_variable_shift(
							wide,
							static_cast<
								test::ShiftOperation>(
								operation),
							fields[0], fields[1],
							fields[2]),
						initial);
				}
			}

	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation < 4U; ++operation)
			for (unsigned flags = 0; flags < 16U; ++flags) {
				auto initial = test::seeded_state(
					0x5580'0000ULL | flags);
				initial.nzcv = flags << 28U;
				test::check_variable_shift(harness,
					shift_names[operation],
					test::encode_variable_shift(wide,
						static_cast<
							test::ShiftOperation>(
							operation),
						7, 3, 5),
					initial);
			}

	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation < 4U; ++operation)
			for (const unsigned value : {3U, 31U})
				for (const unsigned amount : {5U, 31U})
					for (const unsigned destination : {7U, 31U}) {
						auto initial = test::seeded_state(
							0x5600'0000ULL);
						initial.registers[3] =
							0x8000'0000'0000'0001ULL;
						initial.registers[5] = 65U;
						test::check_variable_shift(
							harness,
							shift_names[operation],
							test::encode_variable_shift(
								wide,
								static_cast<
									test::ShiftOperation>(
									operation),
								destination,
								value, amount),
							initial);
					}
}

} // namespace

int main()
{
	encoding_examples();
	engine_tests::EngineHarness harness;
	every_register_field(harness);
	every_effective_shift(harness);
	shift_masking(harness);
	aliases_and_preserved_state(harness);
	harness.print_summary("arm64-base-variable-shift");
	return 0;
}
