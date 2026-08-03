// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_dot_test_support.hpp"

namespace test = arm64_simd_dot_test;

namespace {

constexpr test::Registers registers{28U, 29U, 30U};

void vector_size_allocations()
{
	test::engine_tests::EngineHarness harness;
	for (const auto mode : test::vector_modes)
		for (const bool q : {false, true})
			for (unsigned size = 0; size < 4U; ++size) {
				const auto initial =
					test::state(size + q + static_cast<unsigned>(mode));
				const auto encoded =
					test::vector_instruction(mode, q, size, registers);
				if (size == 2U)
					harness.check_one("dot vector allocated size", encoded,
						initial,
						test::expected_state(initial, mode, q, false,
							registers, 0U),
						test::native_requirements(mode));
				else
					harness.check_illegal("dot vector reserved size", encoded,
						initial, test::native_requirements(mode));
			}
	harness.print_summary("simd-dot-vector-allocation");
}

void indexed_ordinary_size_allocations()
{
	test::engine_tests::EngineHarness harness;
	for (const auto mode :
	     {test::Mode::signed_signed, test::Mode::unsigned_unsigned})
		for (const bool q : {false, true})
			for (unsigned size = 0; size < 4U; ++size) {
				const auto initial =
					test::state(17U + size + q +
						static_cast<unsigned>(mode));
				const auto encoded =
					test::indexed_instruction_with_size(
						mode, q, size, registers, 3U);
				if (size == 2U)
					harness.check_one("dot indexed allocated size", encoded,
						initial,
						test::expected_state(initial, mode, q, true,
							registers, 3U),
						test::native_requirements(mode));
				else
					harness.check_illegal("dot indexed reserved size", encoded,
						initial, test::native_requirements(mode));
			}
	harness.print_summary("simd-dot-indexed-allocation");
}

void scalar_class_is_unallocated()
{
	test::engine_tests::EngineHarness harness;
	for (const auto [form, mode] : test::all_forms)
		harness.check_illegal("dot scalar class is reserved",
			test::instruction(form, mode, true, registers, 2U) |
				0x1000'0000U,
			test::state(41U + static_cast<unsigned>(mode)),
			test::native_requirements(mode));
	harness.print_summary("simd-dot-scalar-holes");
}

void encoding_examples()
{
	test::require(test::vector_instruction(test::Mode::signed_signed, true,
			2U, {0U, 1U, 2U}) == 0x4e82'9420U,
		"SDOT vector encoding mismatch");
	test::require(test::indexed_instruction(test::Mode::unsigned_unsigned,
			false, {3U, 4U, 19U}, 3U) == 0x2fb3'e883U,
		"UDOT indexed encoding mismatch");
	test::require(test::indexed_instruction(test::Mode::signed_unsigned,
			true, {5U, 6U, 18U}, 1U) == 0x4f32'f0c5U,
		"SUDOT indexed encoding mismatch");
	test::require(test::vector_instruction(test::Mode::unsigned_signed,
			false, 2U, {7U, 8U, 9U}) == 0x0e89'9d07U,
		"USDOT vector encoding mismatch");
}

} // namespace

int main()
{
	encoding_examples();
	vector_size_allocations();
	indexed_ordinary_size_allocations();
	scalar_class_is_unallocated();
	return 0;
}
