// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <bit>

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t instruction(bool scalar, bool q,
	bool alternate, unsigned opcode, unsigned encoded, unsigned destination,
	unsigned source)
{
	return (scalar ? 0x5f00'0400U : 0x0f00'0400U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		(alternate ? 0x2000'0000U : 0U) | ((encoded & 127U) << 16) |
		((opcode & 31U) << 11) | ((source & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] std::uint64_t read_lane(const std::array<std::byte, 16> &bytes,
	unsigned offset, unsigned lane_bytes)
{
	std::uint64_t value = 0;
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(bytes[offset + byte])) << (byte * 8U);
	return value;
}

void write_lane(std::array<std::byte, 16> &bytes, unsigned offset,
	unsigned lane_bytes, std::uint64_t value)
{
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		bytes[offset + byte] = static_cast<std::byte>(value >> (byte * 8U));
}

[[nodiscard]] constexpr unsigned __int128 mask_for(unsigned bits)
{
	return bits == 64U ? static_cast<unsigned __int128>(~std::uint64_t{0}) :
		(static_cast<unsigned __int128>(1) << bits) - 1U;
}

[[nodiscard]] constexpr __int128 signed_value(
	std::uint64_t value, unsigned bits)
{
	const auto sign = static_cast<unsigned __int128>(1) << (bits - 1U);
	return static_cast<__int128>((static_cast<unsigned __int128>(value) ^ sign) -
		sign);
}

[[nodiscard]] std::uint64_t expected_lane(std::uint64_t source,
	std::uint64_t old, unsigned bits, unsigned shift, bool alternate,
	unsigned opcode)
{
	const auto mask = mask_for(bits);
	if (opcode == 8U) {
		const auto low_mask = shift == bits ? 0U : mask >> shift;
		return static_cast<std::uint64_t>(
			(old & ~static_cast<std::uint64_t>(low_mask)) |
			(shift == bits ? 0U : source >> shift));
	}
	const bool rounding = opcode == 4U || opcode == 6U;
	std::uint64_t shifted;
	if (alternate) {
		auto value = static_cast<unsigned __int128>(source);
		if (rounding)
			value += static_cast<unsigned __int128>(1) << (shift - 1U);
		shifted = static_cast<std::uint64_t>(value >> shift);
	} else {
		auto value = signed_value(source, bits);
		if (rounding)
			value += static_cast<__int128>(1) << (shift - 1U);
		shifted = static_cast<std::uint64_t>(value >> shift);
	}
	if (opcode == 2U || opcode == 6U)
		shifted += old;
	return static_cast<std::uint64_t>(
		static_cast<unsigned __int128>(shifted) & mask);
}

void every_operation_shape_shift_and_alias()
{
	test::Harness harness;
	for (const bool scalar : {false, true})
		for (const bool q : {false, true}) {
			if (scalar && q)
				continue;
			for (unsigned lane_bits : {8U, 16U, 32U, 64U}) {
				if ((scalar && lane_bits != 64U) ||
				    (!scalar && !q && lane_bits == 64U))
					continue;
				const unsigned vector_bytes = scalar ? 8U : q ? 16U : 8U;
				const unsigned lane_bytes = lane_bits / 8U;
				for (unsigned shift = 1; shift <= lane_bits; ++shift) {
					const unsigned encoded = 2U * lane_bits - shift;
					for (const unsigned opcode : {0U, 2U, 4U, 6U, 8U})
						for (const bool alternate : {false, true}) {
							if (opcode == 8U && !alternate)
								continue;
							for (const bool alias : {false, true}) {
								const unsigned destination = alias ? 20U : 19U;
								const unsigned source = 20U;
								auto initial = test::state(encoded + opcode);
								const auto old_source = initial.vectors[source].bytes;
								const auto old_destination =
									initial.vectors[destination].bytes;
								const auto encoded_instruction = instruction(scalar, q,
									alternate, opcode, encoded, destination, source);
								const auto result = harness.run_interpreter(
									encoded_instruction, initial);
								std::array<std::byte, 16> expected{};
								for (unsigned offset = 0; offset < vector_bytes;
								     offset += lane_bytes)
									write_lane(expected, offset, lane_bytes,
										expected_lane(read_lane(old_source, offset,
											lane_bytes), read_lane(old_destination,
											offset, lane_bytes), lane_bits, shift,
											alternate, opcode));
								test::require(result.vectors[destination].bytes == expected);
								harness.require_differential(
									encoded_instruction, initial);
							}
						}
				}
			}
		}
}

} // namespace

int main()
{
	every_operation_shape_shift_and_alias();
	return 0;
}
