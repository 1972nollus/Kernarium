// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_min_max_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = uml::emulator::tests::base_min_max;
namespace engine_tests = uml::emulator::tests;

namespace {

void every_immediate(engine_tests::EngineHarness &harness,
	std::size_t &cases)
{
	for (const bool wide : {false, true})
		for (const auto description : test::descriptions)
			for (unsigned immediate = 0; immediate < 256U;
			     ++immediate) {
				auto initial = test::state(
					0x4100'0000'0000'0000ULL ^
					(static_cast<std::uint64_t>(immediate)
					 << 32U));
				initial.registers[3] =
					(immediate & 1U) != 0 ?
						0x8000'0000'ffff'ff80ULL :
						0x7fff'ffff'0000'007fULL;
				char name[192];
				std::snprintf(name, sizeof(name),
					"%.*s every-imm8 width=%u encoded=%u",
					static_cast<int>(
						description.immediate_id.size()),
					description.immediate_id.data(),
					wide ? 64U : 32U, immediate);
				test::check(harness, name,
					test::encode_immediate(wide,
						description.operation,
						immediate, 7, 3),
					true, initial);
				++cases;
			}
}

void every_register_pair(engine_tests::EngineHarness &harness,
	std::size_t &cases)
{
	for (const bool wide : {false, true})
		for (const auto description : test::descriptions) {
			const bool signed_operation =
				description.operation ==
					test::Operation::signed_maximum ||
				description.operation ==
					test::Operation::signed_minimum;
			const unsigned immediate =
				signed_operation ? 0x80U : 0xffU;
			for (unsigned rn = 0; rn < 32U; ++rn)
				for (unsigned rd = 0; rd < 32U; ++rd) {
					auto initial = test::state(
						0x5200'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(rn)
						 << 16U) ^
						rd);
					char name[208];
					std::snprintf(name, sizeof(name),
						"%.*s register-fields width=%u rn=%u rd=%u",
						static_cast<int>(description
							.immediate_id.size()),
						description.immediate_id.data(),
						wide ? 64U : 32U, rn, rd);
					test::check(harness, name,
						test::encode_immediate(wide,
							description.operation,
							immediate, rd, rn),
						true, initial);
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
		std::uint64_t{0xffff'ffff'ffff'ff80ULL},
		~std::uint64_t{0},
	};
	for (const bool wide : {false, true})
		for (const auto description : test::descriptions)
			for (const auto value : values)
				for (unsigned immediate = 0; immediate < 256U;
				     ++immediate) {
					auto initial = test::state(
						0x6300'0000'0000'0000ULL ^
						value ^
						(static_cast<std::uint64_t>(
							 immediate)
						 << 40U));
					initial.registers[3] = wide ? value :
						value |
							0xa5a5'5a5a'0000'0000ULL;
					char name[224];
					std::snprintf(name, sizeof(name),
						"%.*s boundary width=%u source=%#llx encoded=%u",
						static_cast<int>(description
							.immediate_id.size()),
						description.immediate_id.data(),
						wide ? 64U : 32U,
						static_cast<unsigned long long>(
							value),
						immediate);
					test::check(harness, name,
						test::encode_immediate(wide,
							description.operation,
							immediate, 7, 3),
						true, initial);
					++cases;
				}
}

} // namespace

int main()
{
	engine_tests::EngineHarness harness;
	std::size_t cases{};
	every_immediate(harness, cases);
	every_register_pair(harness, cases);
	arithmetic_boundaries(harness, cases);
	test::require_counts(harness, cases,
		"arm64-base-min-max-immediate");
	return 0;
}
