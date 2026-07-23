// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <bit>

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t move(bool signed_move, bool q,
	unsigned immediate, unsigned destination, unsigned source)
{
	return (signed_move ? 0x0e00'2c00U : 0x0e00'3c00U) |
		(q ? 0x4000'0000U : 0U) | ((immediate & 31U) << 16) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] std::uint64_t lane_value(
	const uml::backend::arm64::VectorRegister &vector, unsigned offset,
	unsigned bytes)
{
	std::uint64_t value = 0;
	for (unsigned byte = 0; byte < bytes; ++byte)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(vector.bytes[offset + byte])) <<
			(byte * 8U);
	return value;
}

[[nodiscard]] constexpr std::uint64_t sign_extend(
	std::uint64_t value, unsigned bits)
{
	const auto sign = std::uint64_t{1} << (bits - 1U);
	return (value ^ sign) - sign;
}

void every_signed_and_unsigned_lane_width_and_discard()
{
	test::Harness harness;
	for (unsigned immediate = 0; immediate < 32; ++immediate) {
		if ((immediate & 15U) == 0)
			continue;
		const unsigned shift = std::countr_zero(immediate);
		const unsigned bytes = 1U << shift;
		const unsigned offset = (immediate >> (shift + 1U)) << shift;
		for (const bool signed_move : {false, true})
			for (const bool q : {false, true}) {
				const bool valid = signed_move ? shift < (q ? 3U : 2U) :
					(q ? shift == 3U : shift < 3U);
				if (!valid)
					continue;
				for (const unsigned destination : {18U, 31U}) {
					auto initial = test::state(immediate + q * 31U);
					/* Force the sign bit while retaining distinct lane bytes. */
					initial.vectors[17].bytes[offset + bytes - 1U] |=
						std::byte{0x80};
					const auto old_registers = initial.registers;
					const auto instruction = move(signed_move, q, immediate,
						destination, 17);
					const auto result = harness.run_interpreter(
						instruction, initial);
					if (destination == 31U) {
						test::require(result.registers == old_registers);
					} else {
						auto expected = lane_value(
							initial.vectors[17], offset, bytes);
						if (signed_move)
							expected = sign_extend(expected, bytes * 8U);
						if (!q)
							expected &= 0xffff'ffffULL;
						test::require(result.registers[destination] == expected);
					}
					harness.require_differential(instruction, initial);
				}
			}
	}
}

} // namespace

int main()
{
	/* UMOV S->W and D->X are the preferred MOV (to general) aliases. */
	every_signed_and_unsigned_lane_width_and_discard();
	return 0;
}
