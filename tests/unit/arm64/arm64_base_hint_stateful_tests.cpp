// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_hint_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdint>

namespace {

namespace engine_tests = uml::emulator::tests;
namespace gadgets = uml::cpu::arm64::gadgets;

constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

[[nodiscard]] constexpr std::uint64_t stripped_instruction_address(
	std::uint64_t source)
{
	const bool upper =
		(source & (std::uint64_t{1} << 55)) != 0;
	return (source & 0x0000'7fff'ffff'ffffULL) |
		(upper ? 0xffff'8000'0000'0000ULL : 0);
}

void strip_link_register(arm64_hint_test::Harness &harness)
{
	const auto native = arm64_hint_test::primary_requirements(7);
	for (const auto value : {
		0xab55'1234'5678'9abcULL,
		0x12d5'7654'3210'1234ULL,
		0x0000'7fff'ffff'ffffULL,
		0xffff'8000'0000'0001ULL,
	}) {
		auto initial = arm64_system_test::state();
		initial.registers[30] = value;
		auto expected = initial;
		expected.registers[30] =
			stripped_instruction_address(value);
		harness.check("base-hint-strip-link-register",
			arm64_hint_test::primary_instruction(7),
			initial, expected, native);
	}
}

void sign_authenticate_pairs(arm64_hint_test::Harness &harness)
{
	struct Pair final {
		unsigned sign;
		unsigned authenticate;
		unsigned destination;
	};
	constexpr std::array pairs{
		Pair{8, 12, 17},
		Pair{10, 14, 17},
		Pair{24, 28, 30},
		Pair{25, 29, 30},
		Pair{26, 30, 30},
		Pair{27, 31, 30},
	};
	constexpr std::array values{
		0x0000'1234'5678'9abcULL,
		0x0000'4000'0000'1000ULL,
		0xffff'9234'5678'9abcULL,
	};
	const auto native = engine_tests::NativeRequirements::baseline();

	for (const auto pair : pairs)
		for (const auto value : values) {
			auto initial = arm64_system_test::state();
			initial.registers[pair.destination] = value;
			initial.registers[16] =
				0x3456'789a'bcde'f012ULL;
			initial.sp = 0x0000'4567'89ab'cde0ULL;
			auto expected = initial;
			expected.pc = engine_tests::engine_test_code_base +
				3 * sizeof(std::uint32_t);
			const std::array code{
				engine_tests::ProgramWord{
					.offset = 0,
					.instruction =
						arm64_hint_test::primary_instruction(
							pair.sign),
				},
				engine_tests::ProgramWord{
					.offset = sizeof(std::uint32_t),
					.instruction =
						arm64_hint_test::primary_instruction(
							pair.authenticate),
				},
				engine_tests::ProgramWord{
					.offset = 2 * sizeof(std::uint32_t),
					.instruction = terminal_system_call,
				},
			};
			harness.check_program(
				"base-hint-sign-authenticate-round-trip",
				code, initial, expected, native);
		}
}

void modifier_state_alias(arm64_hint_test::Harness &harness)
{
	harness.check("base-hint-modifier-state-alias",
		arm64_hint_test::primary_instruction(39),
		arm64_hint_test::primary_requirements(39));
}

void production_specialization()
{
	const auto pc = engine_tests::engine_test_code_base;
	const auto nop = gadgets::Builder{}.build(
		pc, std::array{arm64_hint_test::primary_instruction(0)});
	for (const unsigned immediate : {
		7U, 8U, 10U, 12U, 14U, 24U, 25U,
		26U, 27U, 28U, 29U, 30U, 31U,
	}) {
		const auto specialized = gadgets::Builder{}.build(
			pc, std::array{
				arm64_hint_test::primary_instruction(immediate)});
		arm64_system_test::require(
			specialized.program.words().front() !=
			nop.program.words().front());
	}
	const auto profile_absent = gadgets::Builder{}.build(
		pc, std::array{arm64_hint_test::primary_instruction(39)});
	arm64_system_test::require(
		profile_absent.program.words().front() ==
		nop.program.words().front());
}

} // namespace

int main()
{
	arm64_hint_test::Harness harness;
	strip_link_register(harness);
	sign_authenticate_pairs(harness);
	modifier_state_alias(harness);
	production_specialization();
	harness.print_summary("arm64-base-hint-stateful");
	return 0;
}
