// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_min_max_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = uml::emulator::tests::base_min_max;
namespace engine_tests = uml::emulator::tests;

namespace {

void every_source_pair(engine_tests::EngineHarness &harness,
	std::size_t &cases)
{
	for (const bool wide : {false, true})
		for (const auto description : test::descriptions)
			for (unsigned rn = 0; rn < 32U; ++rn)
				for (unsigned rm = 0; rm < 32U; ++rm) {
					auto initial = test::state(
						0x7400'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(rn)
						 << 16U) ^
						rm);
					char name[208];
					std::snprintf(name, sizeof(name),
						"%.*s source-fields width=%u rn=%u rm=%u",
						static_cast<int>(description
							.register_id.size()),
						description.register_id.data(),
						wide ? 64U : 32U, rn, rm);
					test::check(harness, name,
						test::encode_register(wide,
							description.operation, 7,
							rn, rm),
						false, initial);
					++cases;
				}
}

void every_destination(engine_tests::EngineHarness &harness,
	std::size_t &cases)
{
	for (const bool wide : {false, true})
		for (const auto description : test::descriptions)
			for (unsigned rd = 0; rd < 32U; ++rd) {
				auto initial = test::state(
					0x8500'0000'0000'0000ULL ^ rd);
				initial.registers[3] =
					0x8000'0000'ffff'ffffULL;
				initial.registers[5] =
					0x7fff'ffff'0000'0001ULL;
				char name[192];
				std::snprintf(name, sizeof(name),
					"%.*s destination-field width=%u rd=%u",
					static_cast<int>(
						description.register_id.size()),
					description.register_id.data(),
					wide ? 64U : 32U, rd);
				test::check(harness, name,
					test::encode_register(wide,
						description.operation, rd, 3,
						5),
					false, initial);
				++cases;
			}
}

void zero_and_discard_combinations(engine_tests::EngineHarness &harness,
	std::size_t &cases)
{
	for (const bool wide : {false, true})
		for (const auto description : test::descriptions)
			for (unsigned combination = 0; combination < 8U;
			     ++combination) {
				const unsigned rd =
					(combination & 4U) != 0 ? 31U : 7U;
				const unsigned rn =
					(combination & 2U) != 0 ? 31U : 3U;
				const unsigned rm =
					(combination & 1U) != 0 ? 31U : 5U;
				auto initial = test::state(
					0x9600'0000'0000'0000ULL ^
					combination);
				char name[208];
				std::snprintf(name, sizeof(name),
					"%.*s zero-discard width=%u rd=%u rn=%u rm=%u",
					static_cast<int>(
						description.register_id.size()),
					description.register_id.data(),
					wide ? 64U : 32U, rd, rn, rm);
				test::check(harness, name,
					test::encode_register(wide,
						description.operation, rd, rn,
						rm),
					false, initial);
				++cases;
			}
}

void destructive_aliases(engine_tests::EngineHarness &harness,
	std::size_t &cases)
{
	for (const bool wide : {false, true})
		for (const auto description : test::descriptions)
			for (unsigned reg = 0; reg < 31U; ++reg) {
				const std::array<std::array<unsigned, 3>, 4>
					fields{{
						{reg, reg, 29U},
						{reg, 28U, reg},
						{27U, reg, reg},
						{reg, reg, reg},
					}};
				for (unsigned form = 0; form < fields.size();
				     ++form) {
					auto initial = test::state(
						0xa700'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(reg)
						 << 8U) ^
						form);
					const auto [rd, rn, rm] = fields[form];
					char name[208];
					std::snprintf(name, sizeof(name),
						"%.*s destructive width=%u reg=%u form=%u",
						static_cast<int>(description
							.register_id.size()),
						description.register_id.data(),
						wide ? 64U : 32U, reg, form);
					test::check(harness, name,
						test::encode_register(wide,
							description.operation,
							rd, rn, rm),
						false, initial);
					++cases;
				}
			}
}

void arithmetic_boundaries(engine_tests::EngineHarness &harness,
	std::size_t &cases)
{
	const std::array values{
		std::uint64_t{0},
		std::uint64_t{1},
		std::uint64_t{0x7f},
		std::uint64_t{0x80},
		std::uint64_t{0xff},
		std::uint64_t{0x7fff'ffffU},
		std::uint64_t{0x8000'0000U},
		std::uint64_t{0xffff'ffffU},
		std::uint64_t{0x7fff'ffff'ffff'ffffULL},
		std::uint64_t{0x8000'0000'0000'0000ULL},
		std::uint64_t{0xffff'ffff'ffff'fffeULL},
		~std::uint64_t{0},
	};
	for (const bool wide : {false, true})
		for (const auto description : test::descriptions)
			for (const auto left : values)
				for (const auto right : values) {
					auto initial = test::state(
						0xb800'0000'0000'0000ULL ^
						left ^ (right << 1U));
					initial.registers[3] = wide ? left :
						left |
							0x5a5a'a5a5'0000'0000ULL;
					initial.registers[5] = wide ? right :
						right |
							0xa5a5'5a5a'0000'0000ULL;
					char name[240];
					std::snprintf(name, sizeof(name),
						"%.*s boundary width=%u left=%#llx right=%#llx",
						static_cast<int>(description
							.register_id.size()),
						description.register_id.data(),
						wide ? 64U : 32U,
						static_cast<unsigned long long>(
							left),
						static_cast<unsigned long long>(
							right));
					test::check(harness, name,
						test::encode_register(wide,
							description.operation, 7,
							3, 5),
						false, initial);
					++cases;
				}
}

} // namespace

int main()
{
	engine_tests::EngineHarness harness;
	std::size_t cases{};
	every_source_pair(harness, cases);
	every_destination(harness, cases);
	zero_and_discard_combinations(harness, cases);
	destructive_aliases(harness, cases);
	arithmetic_boundaries(harness, cases);
	test::require_counts(harness, cases,
		"arm64-base-min-max-register");
	return 0;
}
