// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_two_register_test_support.hpp"

#include <cstdio>
#include <cstdlib>

namespace test = arm64_simd_two_register_test;

namespace {

constexpr test::Registers registers{7U, 11U};

[[noreturn]] void fail(const char *message)
{
	std::fprintf(stderr, "%s\n", message);
	std::abort();
}

void encoding_examples()
{
	if (test::instruction(true, false, false, 3, 11, {0, 1}) !=
	    0x5ee0'b820U)
		fail("ABS scalar encoding mismatch");
	if (test::instruction(false, false, false, 0, 5, {0, 1}) !=
	    0x0e20'5820U)
		fail("CNT vector encoding mismatch");
	if (test::instruction(false, true, true, 0, 19, {0, 1}) !=
	    0x6e21'3820U)
		fail("SHLL2 vector encoding mismatch");
}

void check_tuple(test::engine_tests::EngineHarness &harness, bool scalar,
	bool q, bool unsigned_group, unsigned size, unsigned opcode)
{
	const auto encoded = test::instruction(
		scalar, q, unsigned_group, size, opcode, registers);
	const auto initial = test::state(
		encoded ^ (scalar ? 0x513U : 0xa27U));
	char name[160];
	std::snprintf(name, sizeof(name),
		"simd-two-register-allocation scalar=%u q=%u u=%u size=%u opcode=%u",
		scalar ? 1U : 0U, q ? 1U : 0U,
		unsigned_group ? 1U : 0U, size, opcode);
	if (test::allocated(scalar, q, unsigned_group, size, opcode)) {
		harness.check_one(name, encoded, initial,
			test::expected_state(initial, scalar, q, unsigned_group,
				size, opcode, registers),
			test::native_requirements());
	} else {
		harness.check_illegal(
			name, encoded, initial, test::native_requirements());
	}
}

void exhaustive_allocation(test::engine_tests::EngineHarness &harness)
{
	for (const auto opcode : test::target_opcodes) {
		for (const bool q : {false, true})
			for (const bool unsigned_group : {false, true})
				for (unsigned size = 0; size < 4U; ++size)
					check_tuple(harness, false, q,
						unsigned_group, size, opcode);
		for (const bool unsigned_group : {false, true})
			for (unsigned size = 0; size < 4U; ++size)
				check_tuple(harness, true, false,
					unsigned_group, size, opcode);
	}
}

} // namespace

int main()
{
	encoding_examples();
	test::engine_tests::EngineHarness harness;
	exhaustive_allocation(harness);
	harness.print_summary("arm64-simd-two-register-allocation");
	return 0;
}
