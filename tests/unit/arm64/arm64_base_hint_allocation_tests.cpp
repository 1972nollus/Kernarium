// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_hint_test_support.hpp"

#include <cstdint>
#include <cstdio>

namespace {

namespace engine_tests = uml::emulator::tests;
using arm64_hint_test::ExpectedCounts;
using arm64_hint_test::Harness;

void complete_owned_primary_space(Harness &harness,
	ExpectedCounts &expected)
{
	for (unsigned immediate = 0; immediate < 128U; ++immediate) {
		if (!arm64_hint_test::is_owned_primary_hint(immediate))
			continue;
		const auto instruction =
			arm64_hint_test::primary_instruction(immediate);
		auto initial = arm64_hint_test::seeded_state(
			0x5100'0000'0000'0000ULL | immediate);
		char name[144];
		std::snprintf(name, sizeof(name),
			"BASE-0175 HINT primary immediate=%u allocation=%s",
			immediate,
			arm64_hint_test::is_allocated_primary_hint(immediate) ?
				"allocated" : "unallocated-nop");
		const auto native =
			arm64_hint_test::primary_requirements(immediate);
		harness.check_stateless(name, instruction, initial, native);
		expected.add(native);
	}
}

void speculation_barrier_allocation(Harness &harness,
	ExpectedCounts &expected)
{
	const auto native = engine_tests::NativeRequirements::features(
		::uml::cpu::ArmFeature::sb);
	auto initial = arm64_hint_test::seeded_state(
		0x5200'0000'0000'0000ULL);
	harness.check_stateless("BASE-0356 SB allocated",
		arm64_hint_test::speculation_barrier_instruction,
		initial, native);
	expected.add(native);

	/*
	 * CRm is fixed to zero for SB. The other fifteen members of the same
	 * barrier opcode allocation are reserved in the 2026-06 profile and must
	 * be rejected before they can mutate any architectural state.
	 */
	for (unsigned crm = 1; crm < 16U; ++crm) {
		const auto instruction =
			arm64_hint_test::speculation_barrier_instruction |
			(crm << 8);
		initial = arm64_hint_test::seeded_state(
			0x5210'0000'0000'0000ULL | crm);
		char name[112];
		std::snprintf(name, sizeof(name),
			"BASE-0356 SB reserved CRm=%u", crm);
		harness.check_reserved(name, instruction, initial);
		expected.add(engine_tests::NativeRequirements::baseline(), true);
	}
}

void gsb_allocation(Harness &harness, ExpectedCounts &expected)
{
	const auto native = engine_tests::NativeRequirements::features(
		::uml::cpu::ArmFeature::gcie);
	for (unsigned operation = 0; operation < 2U; ++operation)
		for (unsigned transfer = 0; transfer < 32U; ++transfer) {
			const auto instruction = arm64_hint_test::gsb_instruction(
				operation, transfer);
			const auto initial = arm64_hint_test::seeded_state(
				0x5300'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(operation) << 8) |
				transfer);
			char name[128];
			std::snprintf(name, sizeof(name),
				"BASE-0174 GSB operation=%s Rt=%u EL0 profile",
				operation == 0U ? "SYS" : "ACK", transfer);
			/*
			 * The static userspace profile has no virtual interrupt
			 * controller state, so allocated GSB operations trap at EL0.
			 * This is a profile exception, not a reserved encoding: JIT
			 * eligibility is still gated by FEAT_GCIE.
			 */
			harness.check_profile_illegal(
				name, instruction, initial, native);
			expected.add(native);
		}

	for (unsigned operation = 2; operation < 8U; ++operation)
		for (unsigned transfer = 0; transfer < 32U; ++transfer) {
			const auto instruction = arm64_hint_test::gsb_instruction(
				operation, transfer);
			const auto initial = arm64_hint_test::seeded_state(
				0x5310'0000'0000'0000ULL |
				(static_cast<std::uint64_t>(operation) << 8) |
				transfer);
			char name[128];
			std::snprintf(name, sizeof(name),
				"BASE-0174 GSB reserved op2=%u Rt=%u",
				operation, transfer);
			harness.check_reserved(name, instruction, initial);
			expected.add(engine_tests::NativeRequirements::baseline(),
				true);
		}
}

} // namespace

int main()
{
	Harness harness;
	ExpectedCounts expected;
	complete_owned_primary_space(harness, expected);
	speculation_barrier_allocation(harness, expected);
	gsb_allocation(harness, expected);
	arm64_hint_test::require_counts(harness, expected);
	harness.print_summary("arm64-base-hint-allocation");
	return 0;
}
