// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_shift_immediate_test_support.hpp"

#include <array>

namespace test = arm64_simd_shift_immediate_test;

namespace {

constexpr std::array forms{
	test::Form::vector_64,
	test::Form::vector_128,
	test::Form::scalar,
};

void every_owned_allocation()
{
	test::engine_tests::EngineHarness harness;
	for (const auto form : forms) {
		const unsigned first_encoded =
			form == test::Form::scalar ? 0U : 8U;
		for (unsigned encoded = first_encoded; encoded < 128U; ++encoded)
			for (const bool unsigned_group : {false, true})
				for (unsigned opcode = 0; opcode < 32U; ++opcode) {
					/* Opcode 20 is the vector shift-left-long family.
					 * Opcodes 28 and 31 are fixed-point conversions. Their
					 * allocations are owned by separate suites. */
					if ((opcode == 20U &&
					     form != test::Form::scalar) ||
					    opcode == 28U || opcode == 31U)
						continue;
					const auto instruction = test::raw_instruction(form,
						unsigned_group, opcode, encoded, 2U, 3U);
					const auto initial = test::state(encoded +
						opcode * 7U + unsigned_group * 29U +
						static_cast<unsigned>(form) * 53U);
					const auto operation =
						test::operation_for(unsigned_group, opcode);
					if (operation &&
					    test::valid(*operation, form, encoded)) {
						harness.check_one(
							"shift-immediate allocated encoding",
							instruction, initial,
							test::expected_state(initial, form,
								*operation, encoded, 2U, 3U),
							test::native_requirements());
					} else {
						harness.check_illegal(
							"shift-immediate reserved encoding",
							instruction, initial,
							test::native_requirements());
					}
				}
	}
	harness.print_summary("simd-shift-immediate-allocation");
}

} // namespace

int main()
{
	every_owned_allocation();
	return 0;
}
