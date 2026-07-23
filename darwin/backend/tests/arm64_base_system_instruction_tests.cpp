// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_system_test_support.hpp"

#include <array>

namespace test = arm64_system_test;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

[[nodiscard]] constexpr bool enabled_cache(std::uint32_t instruction)
{
	const auto operation = instruction & 0xffff'ffe0U;
	return operation == 0xd50b'7b20U || operation == 0xd50b'7520U;
}

void enabled_cache_operations()
{
	test::Harness harness;
	for (unsigned transfer = 0; transfer < 32; ++transfer) {
		for (const auto instruction : {
			0xd50b'7b20U | transfer, // DC CVAU, Xt
			0xd50b'7520U | transfer  // IC IVAU, Xt
		}) {
			const auto initial = test::state();
			auto [interpreted, memory] = harness.run_interpreter(instruction);
			auto expected = initial;
			expected.pc = test::pc + 4;
			test::require_unchanged(expected, interpreted);
			test::require(memory.reads == 0 && memory.writes == 0);
			if ((instruction & 0xffff'ffe0U) == 0xd50b'7520U) {
				test::require(memory.invalidations.size() == 1);
				test::require(memory.invalidations.front() ==
					initial.read_register(transfer, false));
			} else {
				test::require(memory.invalidations.empty());
			}

			auto gadget = harness.run_gadget(instruction);
			expected.pc = test::pc + 8;
			test::require_unchanged(expected, gadget);
		}
	}
}

void complete_numeric_allocation()
{
	test::Harness harness;
	/* Every numeric SYS/SYSL operation is decoded.  Exercise every operation
	 * tuple and both a real register and the architectural XZR/default form in
	 * both runpaths.  The only enabled tuples are the two userspace cache
	 * synchronization operations tested above. */
	for (const bool result : {false, true})
		for (unsigned op1 = 0; op1 < 8; ++op1)
			for (unsigned crn = 0; crn < 16; ++crn)
				for (unsigned crm = 0; crm < 16; ++crm)
					for (unsigned op2 = 0; op2 < 8; ++op2)
						for (const unsigned transfer : {0U, 31U}) {
							const auto instruction = test::system_instruction(
								result, op1, crn, crm, op2, transfer);
							if (!enabled_cache(instruction))
								harness.require_illegal(instruction);
						}
}

void production_specialization()
{
#if !UML_TEST_GADGET_INTERPRETER_RUNPATH
	const auto dc = gadgets::Builder{}.build(test::pc,
		std::array{0xd50b'7b20U, 0xd400'0001U});
	const auto ic = gadgets::Builder{}.build(test::pc,
		std::array{0xd50b'7520U, 0xd400'0001U});
	const auto illegal = gadgets::Builder{}.build(test::pc,
		std::array{test::system_instruction(false, 7, 15, 15, 7, 31),
			0xd400'0001U});
	test::require(dc.program.words().front() == ic.program.words().front());
	test::require(dc.program.words().front() != illegal.program.words().front());
	test::require(illegal.program.instruction_count() == 1);
#endif
}

} // namespace

int main()
{
	enabled_cache_operations();
	complete_numeric_allocation();
	production_specialization();
	return 0;
}
