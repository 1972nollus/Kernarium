// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_two_register_test_support.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace test = arm64_simd_two_register_test;

namespace {

constexpr std::array bit_opcodes{0U, 1U, 5U};
constexpr test::Registers registers{0U, 1U};

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

void exhaustive_register_fields(test::engine_tests::EngineHarness &harness)
{
	test::for_each_allocated_form(bit_opcodes,
		[&](bool scalar, bool q, bool unsigned_group,
		    unsigned size, unsigned opcode) {
			test::check_form(harness, "simd-two-register-bit",
				scalar, q, unsigned_group, size, opcode);
		});
}

void known_answers(test::engine_tests::EngineHarness &harness)
{
	for (const bool q : {false, true})
		for (const bool unsigned_group : {false, true})
			for (unsigned size = 0; size < 4U; ++size)
				for (const auto opcode : bit_opcodes) {
					if (!test::allocated(false, q, unsigned_group,
					    size, opcode))
						continue;
					auto initial = test::state(
						opcode * 31U + size * 17U +
						unsigned_group * 7U + q);
					constexpr std::array pattern{
						0x81U, 0x0fU, 0xf0U, 0x55U,
						0x01U, 0x80U, 0xaaU, 0x33U,
						0x7fU, 0x40U, 0x20U, 0x10U,
						0x08U, 0x04U, 0x02U, 0x01U};
					for (unsigned byte = 0; byte < pattern.size(); ++byte)
						initial.vectors[registers.source].bytes[byte] =
							static_cast<std::byte>(pattern[byte]);
					const auto expected = test::expected_state(
						initial, false, q, unsigned_group,
						size, opcode, registers);
					if (opcode == 5U && !unsigned_group)
						require(
							expected.vectors[0].bytes[0] ==
								std::byte{2},
							"CNT oracle mismatch");
					if (opcode == 5U && unsigned_group && size == 0U)
						require(
							expected.vectors[0].bytes[0] ==
								std::byte{0x7e},
							"NOT/MVN oracle mismatch");
					if (opcode == 5U && unsigned_group && size == 1U)
						require(
							expected.vectors[0].bytes[1] ==
								std::byte{0xf0},
							"RBIT oracle mismatch");
					const auto name = opcode == 5U && unsigned_group &&
						size == 0U ? "FPSIMD-0257 NOT MVN" :
						"simd-bit-known-answer";
					harness.check_one(name,
						test::instruction(false, q,
							unsigned_group, size, opcode,
							registers),
						initial, expected,
						test::native_requirements());
				}
}

void alias_encoding()
{
	const auto not_encoding =
		test::instruction(false, true, true, 0, 5, {17U, 23U});
	const auto mvn_encoding =
		test::instruction(false, true, true, 0, 5, {17U, 23U});
	require(not_encoding == mvn_encoding,
		"MVN alias does not share the NOT encoding");
}

} // namespace

int main()
{
	alias_encoding();
	test::engine_tests::EngineHarness harness;
	exhaustive_register_fields(harness);
	known_answers(harness);
	harness.print_summary("arm64-simd-two-register-bit");
	return 0;
}
