// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_hint_test_support.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

namespace engine_tests = uml::backend::tests;
using arm64_hint_test::ExpectedCounts;
using arm64_hint_test::Harness;

struct PrimaryVariant final {
	const char *name;
	unsigned immediate;
};

constexpr std::array primary_variants{
	PrimaryVariant{"BASE-0175 HINT NOP", 0},
	PrimaryVariant{"BASE-0516 YIELD", 1},
	PrimaryVariant{"BASE-0510 WFE", 2},
	PrimaryVariant{"BASE-0512 WFI", 3},
	PrimaryVariant{"BASE-0372 SEV", 4},
	PrimaryVariant{"BASE-0373 SEVL", 5},
	PrimaryVariant{"BASE-0149 DGH", 6},
	PrimaryVariant{"BASE-0159 ESB", 16},
	PrimaryVariant{"BASE-0324 PSB CSYNC", 17},
	PrimaryVariant{"BASE-0491 TSB CSYNC", 18},
	PrimaryVariant{"BASE-0161 GCSB DSYNC", 19},
	PrimaryVariant{"BASE-0137 CSDB", 20},
	PrimaryVariant{"BASE-0088 CLRBHB", 22},
	PrimaryVariant{"BASE-0050 BTI", 32},
	PrimaryVariant{"BASE-0050 BTI c", 34},
	PrimaryVariant{"BASE-0050 BTI j", 36},
	PrimaryVariant{"BASE-0050 BTI jc", 38},
	PrimaryVariant{"BASE-0374 SHUH", 50},
	PrimaryVariant{"BASE-0374 SHUH PH", 51},
	PrimaryVariant{"BASE-0175 HINT unallocated upper boundary", 127},
};

void full_state_primary_behavior(Harness &harness,
	ExpectedCounts &expected)
{
	constexpr std::array seeds{
		0x6100'0000'0000'0001ULL,
		0x6200'ffff'0000'aaaaULL,
		0x63ff'0000'5555'1234ULL,
	};
	for (const auto variant : primary_variants)
		for (unsigned pattern = 0; pattern < seeds.size(); ++pattern) {
			const auto instruction =
				arm64_hint_test::primary_instruction(
					variant.immediate);
			auto initial = arm64_hint_test::seeded_state(
				seeds[pattern] ^
				(static_cast<std::uint64_t>(
					variant.immediate) << 23));
			char name[176];
			std::snprintf(name, sizeof(name), "%s full-state pattern=%u",
				variant.name, pattern);
			const auto native =
				arm64_hint_test::primary_requirements(
					variant.immediate);
			harness.check_stateless(name, instruction, initial, native);
			expected.add(native);
		}
}

void event_sequence_can_make_progress(Harness &harness,
	ExpectedCounts &expected)
{
	/*
	 * The userspace execution model permits WFE/WFI to complete immediately.
	 * SEV/SEVL therefore need no hidden event-register state, but the complete
	 * sequence must still retire in order and reach the terminating syscall.
	 */
	constexpr std::array code{
		engine_tests::ProgramWord{
			.offset = 0,
			.instruction = arm64_hint_test::primary_instruction(5),
		},
		engine_tests::ProgramWord{
			.offset = 4,
			.instruction = arm64_hint_test::primary_instruction(2),
		},
		engine_tests::ProgramWord{
			.offset = 8,
			.instruction = arm64_hint_test::primary_instruction(4),
		},
		engine_tests::ProgramWord{
			.offset = 12,
			.instruction = arm64_hint_test::primary_instruction(2),
		},
		engine_tests::ProgramWord{
			.offset = 16,
			.instruction = arm64_hint_test::primary_instruction(3),
		},
		engine_tests::ProgramWord{
			.offset = 20,
			.instruction = arm64_hint_test::primary_instruction(1),
		},
		engine_tests::ProgramWord{
			.offset = 24,
			.instruction = arm64_hint_test::primary_instruction(20),
		},
		engine_tests::ProgramWord{
			.offset = 28,
			.instruction = 0xd400'0001U,
		},
	};
	const auto initial =
		arm64_hint_test::seeded_state(0x6400'1234'5678'9abcULL);
	auto oracle = initial;
	oracle.pc = engine_tests::engine_test_code_base + code.size() * 4U;
	const auto native = engine_tests::NativeRequirements::baseline();
	harness.check_program(
		"BASE-0510/0512/0516 event and yield progress sequence",
		code, initial, oracle, native);
	expected.add(native);
}

void speculation_barrier_full_state(Harness &harness,
	ExpectedCounts &expected)
{
	const auto native = engine_tests::NativeRequirements::features(
		uml::backend::ArmFeature::sb);
	for (unsigned pattern = 0; pattern < 4U; ++pattern) {
		const auto initial = arm64_hint_test::seeded_state(
			0x6500'0000'0000'0000ULL |
			(static_cast<std::uint64_t>(pattern) << 36));
		char name[96];
		std::snprintf(name, sizeof(name),
			"BASE-0356 SB full-state pattern=%u", pattern);
		harness.check_stateless(name,
			arm64_hint_test::speculation_barrier_instruction,
			initial, native);
		expected.add(native);
	}
}

void gsb_profile_exception_preserves_state(Harness &harness,
	ExpectedCounts &expected)
{
	const auto native = engine_tests::NativeRequirements::features(
		uml::backend::ArmFeature::gcie);
	for (unsigned operation = 0; operation < 2U; ++operation)
		for (const unsigned transfer : {0U, 16U, 30U, 31U}) {
			const auto instruction = arm64_hint_test::gsb_instruction(
				operation, transfer);
			const auto initial = arm64_hint_test::seeded_state(
				0x6600'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(operation) << 12) |
				transfer);
			char name[128];
			std::snprintf(name, sizeof(name),
				"BASE-0174 GSB %s Rt=%u profile exception state",
				operation == 0U ? "SYS" : "ACK", transfer);
			harness.check_profile_illegal(
				name, instruction, initial, native);
			expected.add(native);
		}
}

} // namespace

int main()
{
	Harness harness;
	ExpectedCounts expected;
	full_state_primary_behavior(harness, expected);
	event_sequence_can_make_progress(harness, expected);
	speculation_barrier_full_state(harness, expected);
	gsb_profile_exception_preserves_state(harness, expected);
	arm64_hint_test::require_counts(harness, expected);
	harness.print_summary("arm64-base-hint-behavior");
	return 0;
}
