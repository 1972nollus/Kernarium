// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_move_wide_test_support.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace test = arm64_base_move_wide_test;

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

[[nodiscard]] constexpr bool alias_is_preferred(
	bool wide, unsigned halfword, unsigned immediate) noexcept
{
	if (!wide && immediate == 0xffffU)
		return false;
	return immediate != 0U || halfword == 0U;
}

void every_preferred_immediate(
	test::engine_tests::EngineHarness &harness, std::size_t &cases)
{
	for (const bool wide : {false, true})
		for (unsigned halfword = 0; halfword < (wide ? 4U : 2U);
		     ++halfword)
			for (unsigned immediate = 0; immediate < 65'536U;
			     ++immediate) {
				if (!alias_is_preferred(
					    wide, halfword, immediate))
					continue;
				auto initial = test::state(static_cast<unsigned>(
					0x4100'0000U + cases));
				initial.registers[7] ^=
					static_cast<std::uint64_t>(immediate) *
					0x0001'0001'0001'0001ULL;
				char name[144];
				std::snprintf(name, sizeof(name),
					"BASE-0284 MOV inverted alias width=%u "
					"halfword=%u imm16=%u",
					wide ? 64U : 32U, halfword, immediate);
				test::check(harness, name, wide, 0U, halfword,
					immediate, 7U, initial);
				++cases;
			}
}

void every_destination_field(
	test::engine_tests::EngineHarness &harness, std::size_t &cases)
{
	constexpr std::array immediates{
		0U, 1U, 0x7fffU, 0x8000U, 0xffffU,
	};
	for (const bool wide : {false, true})
		for (unsigned halfword = 0; halfword < (wide ? 4U : 2U);
		     ++halfword)
			for (const auto immediate : immediates) {
				if (!alias_is_preferred(
					    wide, halfword, immediate))
					continue;
				for (unsigned destination = 0; destination < 32U;
				     ++destination) {
					auto initial = test::state(
						static_cast<unsigned>(
							0x4200'0000U + cases));
					char name[144];
					std::snprintf(name, sizeof(name),
						"BASE-0284 MOV inverted alias rd=%u "
						"width=%u halfword=%u imm16=%u",
						destination, wide ? 64U : 32U,
						halfword, immediate);
					test::check(harness, name, wide, 0U,
						halfword, immediate, destination,
						initial);
					++cases;
				}
			}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	std::size_t cases = 0;
	every_preferred_immediate(harness, cases);
	every_destination_field(harness, cases);
	require(cases == 393'978U,
		"MOV inverted alias case count mismatch");
	harness.print_summary("arm64-base-mov-inverted-alias");
	return 0;
}
