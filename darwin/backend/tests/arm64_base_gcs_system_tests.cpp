// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_system_test_support.hpp"

#include <array>

namespace test = arm64_system_test;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

void system_aliases()
{
	test::Harness harness;
	struct Alias final {
		bool result;
		unsigned op1;
		unsigned op2;
	};
	/* GCSPOPCX, GCSPOPM, GCSPOPX, GCSPUSHM, GCSPUSHX, GCSSS1, and
	 * GCSSS2 respectively. */
	constexpr std::array aliases{
		Alias{false, 0, 5}, Alias{true, 3, 1}, Alias{false, 0, 6},
		Alias{false, 3, 0}, Alias{false, 0, 4}, Alias{false, 3, 2},
		Alias{true, 3, 3},
	};
	for (const auto alias : aliases)
		for (unsigned transfer = 0; transfer < 32; ++transfer)
			harness.require_illegal(test::system_instruction(alias.result,
				alias.op1, 7, 7, alias.op2, transfer));
}

void direct_store_encodings()
{
	test::Harness harness;
	/* Exhaust every Rt/Rn combination for GCSSTR and GCSSTTR.  FEAT_GCS is
	 * absent, so Undefined precedes source-register reads, SP alignment, and
	 * memory address formation in the strict interpreter and in production. */
	for (const auto base : {0xd91f'0c00U, 0xd91f'1c00U})
		for (unsigned base_register = 0; base_register < 32; ++base_register)
			for (unsigned transfer = 0; transfer < 32; ++transfer)
				harness.require_illegal(base | (base_register << 5) |
					transfer);
}

void feature_absent_barrier_is_hint()
{
	/* Unlike GCS state/memory operations, GCSB is defined to become a NOP
	 * when FEAT_GCS is absent. */
	test::Harness harness;
	const auto initial = test::state();
	auto [interpreted, memory] = harness.run_interpreter(0xd503'227fU);
	auto expected = initial;
	expected.pc = test::pc + 4;
	test::require_unchanged(expected, interpreted);
	test::require(memory.reads == 0 && memory.writes == 0 &&
		memory.invalidations.empty());
	auto gadget = harness.run_gadget(0xd503'227fU);
	expected.pc = test::pc + 8;
	test::require_unchanged(expected, gadget);
}

void production_feature_gate()
{
#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
	const auto sys = gadgets::Builder{}.build(test::pc,
		std::array{test::system_instruction(false, 0, 7, 7, 5, 31),
			0xd400'0001U});
	const auto direct = gadgets::Builder{}.build(test::pc,
		std::array{0xd91f'0c00U, 0xd400'0001U});
	test::require(sys.program.words().front() == direct.program.words().front());
	test::require(sys.program.instruction_count() == 1 &&
		direct.program.instruction_count() == 1);
#endif
}

} // namespace

int main()
{
	system_aliases();
	direct_store_encodings();
	feature_absent_barrier_is_hint();
	production_feature_gate();
	return 0;
}
