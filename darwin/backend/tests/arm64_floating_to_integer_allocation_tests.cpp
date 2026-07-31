// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_to_integer_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_floating_to_integer_test;

namespace {

void every_valid_encoding_and_register_allocation(
	test::engine_tests::EngineHarness &harness)
{
	unsigned forms{};
	unsigned seed{};
	test::for_each_form([&](const test::Form &form) {
		++forms;
		for (const auto registers : test::register_edges) {
			auto initial = test::state(++seed);
			const std::array input{
				test::power_of_two(form.source_width, 0)};
			test::fill_source(initial, form, registers, input);
			char name[192];
			std::snprintf(name, sizeof(name),
				"floating-to-integer-allocation destination=%u rounding=%u "
				"unsigned=%u scalar=%u fixed=%u source=%u integer=%u "
				"bytes=%u fraction=%u rd=%u rn=%u",
				static_cast<unsigned>(form.destination),
				static_cast<unsigned>(form.rounding),
				static_cast<unsigned>(form.unsigned_result),
				static_cast<unsigned>(form.scalar),
				static_cast<unsigned>(form.fixed), form.source_width,
				form.integer_width, form.vector_bytes,
				form.fractional_bits, registers.destination,
				registers.source);
			test::check(harness, name, form, registers, initial);
		}
	});
	if (forms != 1'300U)
		__builtin_trap();
}

void reserved_and_unallocated_encodings(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 20'000U;
	const auto illegal = [&](const char *name, std::uint32_t instruction) {
		harness.check_illegal(name, instruction, test::state(++seed));
	};

	for (const bool sf : {false, true}) {
		for (const unsigned rmode : {0U, 1U, 2U, 3U})
			for (const unsigned opcode : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U}) {
				const auto word = 0x1e20'0000U |
					(sf ? 0x8000'0000U : 0U) | (2U << 22) |
					(rmode << 19) | (opcode << 16) | (7U << 5) | 6U;
				/* Some opcode/rmode pairs are allocated to integer-to-FP
				 * or general moves and therefore belong to other rows. */
				const bool conversion_member =
					opcode <= 1U || (opcode >= 4U && rmode != 0U) ||
					(opcode >= 2U && opcode <= 3U && rmode != 0U) ||
					((opcode == 4U || opcode == 5U) && rmode == 0U);
				/* The 64-bit rmode=01/opcode=110,111 words are the
				 * allocated upper-lane general-register moves, not
				 * members of this conversion allocation. */
				const bool upper_lane_move =
					sf && rmode == 1U && opcode >= 6U;
				if (conversion_member && !upper_lane_move)
					illegal("floating-to-integer-reserved-type", word);
			}
	}

	for (const auto rounding : {test::Rounding::tie_even,
	     test::Rounding::plus_infinity, test::Rounding::minus_infinity,
	     test::Rounding::zero, test::Rounding::tie_away}) {
		for (const bool unsigned_result : {false, true}) {
			for (const auto widths : std::array{
			     std::array{32U, 32U}, std::array{64U, 64U}}) {
				const test::Form invalid{
					test::Destination::simd_cross_width, rounding,
					unsigned_result, true, false, widths[0], widths[1],
					widths[1] / 8U, 0U};
				illegal("floating-to-integer-cross-width-required",
					test::instruction(invalid, {3U, 4U}));
			}
		}
	}

	for (const auto word : std::array{
	     0x1e2c'00e6U, 0x1e3c'00e6U, 0x1e2e'00e6U,
	     0x1e3e'00e6U})
		illegal("floating-to-integer-reserved-opcode-rmode", word);

	for (const bool unsigned_result : {false, true}) {
		for (const unsigned source_width : {16U, 32U, 64U}) {
			const test::Form invalid{
				test::Destination::gpr, test::Rounding::zero,
				unsigned_result, true, true, source_width, 32U,
				source_width / 8U, 33U};
			illegal("floating-to-integer-fixed-gpr-scale-out-of-range",
				test::instruction(invalid, {8U, 9U}));
		}
		const auto signedness = unsigned_result ? 0x2000'0000U : 0U;
		illegal("floating-to-integer-fixed-simd-immh-reserved",
			0x0f0f'fc00U | signedness | (9U << 5) | 8U);
		illegal("floating-to-integer-fixed-simd-d-requires-q",
			0x0fc0'fc00U | signedness | (9U << 5) | 8U);
		illegal("floating-to-integer-simd-d-requires-q",
			0x0e61'a800U | signedness | (9U << 5) | 8U);
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_valid_encoding_and_register_allocation(harness);
	reserved_and_unallocated_encodings(harness);
	harness.print_summary("arm64-floating-to-integer-allocation");
	return 0;
}
