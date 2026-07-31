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

void reserved_select_bit(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const auto description : test::select_descriptions)
			for (unsigned condition = 0; condition < 16U; ++condition) {
				const unsigned rd = (condition * 3U + 7U) & 31U;
				const unsigned rn = (condition * 5U + 11U) & 31U;
				const unsigned rm = (condition * 7U + 13U) & 31U;
				auto initial = test::state(
					0x8800'0000'0000'0000ULL ^
					condition);
				initial.nzcv = condition << 28U;
				char name[176];
				std::snprintf(name, sizeof(name),
					"%.*s reserved-o2 width=%u cond=%u rd=%u rn=%u rm=%u",
					static_cast<int>(description.id.size()),
					description.id.data(),
					wide ? 64U : 32U, condition, rd,
					rn, rm);
				harness.check_illegal(name,
					test::encode_select(wide,
						description.operation, condition,
						rd, rn, rm) |
						0x0000'0800U,
					initial,
					engine_tests::NativeRequirements::baseline());
				++cases;
			}
}

void reserved_compare_bits(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const bool subtract : {false, true})
			for (const bool immediate : {false, true})
				for (unsigned condition = 0; condition < 16U;
				     ++condition)
					for (const std::uint32_t reserved :
					     {0x0000'0400U,
					      0x0000'0010U,
					      0x0000'0410U}) {
						const unsigned rn =
							(condition * 3U + 31U) &
							31U;
						const unsigned operand =
							(condition * 11U + 5U) &
							31U;
						auto initial = test::state(
							0x9900'0000'0000'0000ULL ^
							(static_cast<std::uint64_t>(
								 reserved)
							 << 20U) ^
							condition);
						initial.nzcv =
							(condition ^ 9U) << 28U;
						const auto id =
							test::compare_id(
								subtract,
								immediate);
						char name[192];
						std::snprintf(name,
							sizeof(name),
							"%.*s reserved=%#x width=%u cond=%u rn=%u operand=%u",
							static_cast<int>(
								id.size()),
							id.data(), reserved,
							wide ? 64U : 32U,
							condition, rn,
							operand);
						harness.check_illegal(name,
							test::encode_compare(
								wide, subtract,
								immediate,
								condition,
								condition,
								rn, operand) |
								reserved,
							initial,
							engine_tests::
								NativeRequirements::
									baseline());
						++cases;
					}
}

/*
 * AL/NV are excluded only from the preferred aliases.  Their bit patterns are
 * still allocated CSINC/CSINV/CSNEG encodings and must not be rejected.
 */
void alias_exclusion_is_not_unallocated(
	engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (const auto operation :
		     {test::SelectOperation::increment,
		      test::SelectOperation::invert,
		      test::SelectOperation::negate})
			for (const unsigned condition : {14U, 15U})
				for (const std::array<unsigned, 2> sources :
				     {std::array{5U, 5U},
				      std::array{31U, 31U}}) {
					auto initial = test::state(
						0xaa00'0000'0000'0000ULL ^
						(static_cast<std::uint64_t>(
							 condition)
						 << 32U));
					initial.nzcv = 0;
					char name[160];
					std::snprintf(name, sizeof(name),
						"alias-excluded-but-allocated width=%u op=%u cond=%u source=%u",
						wide ? 64U : 32U,
						static_cast<unsigned>(
							operation),
						condition, sources[0]);
					test::check_select(harness, name,
						test::encode_select(wide,
							operation, condition,
							7, sources[0],
							sources[1]),
						initial);
					++cases;
				}
}

} // namespace

int main()
{
	engine_tests::EngineHarness harness;
	reserved_select_bit(harness);
	reserved_compare_bits(harness);
	alias_exclusion_is_not_unallocated(harness);
	const auto &counts = harness.counts();
	require(counts.interpreter == cases && counts.gadgets == cases,
		"allocation suite did not execute every case in both software engines");
	require(counts.jit + counts.jit_skipped == cases,
		"allocation suite did not execute or explicitly skip every JIT case");
	harness.print_summary("arm64-base-condition-allocation");
	return 0;
}
