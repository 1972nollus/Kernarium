// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <algorithm>

namespace test = arm64_simd_test;

namespace {

enum class Mode { signed_signed, signed_unsigned, unsigned_unsigned };

[[nodiscard]] constexpr std::uint32_t instruction(bool scalar, bool high,
	bool alternate, unsigned opcode, unsigned encoded, unsigned destination,
	unsigned source)
{
	return (scalar ? 0x5f00'0400U : 0x0f00'0400U) |
		(!scalar && high ? 0x4000'0000U : 0U) |
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

[[nodiscard]] constexpr __int128 sign_extend(
	std::uint64_t value, unsigned bits)
{
	const auto sign = static_cast<unsigned __int128>(1) << (bits - 1U);
	return static_cast<__int128>((static_cast<unsigned __int128>(value) ^ sign) -
		sign);
}

void every_operation_width_shift_part_and_scalar_isolation()
{
	test::Harness harness;
	for (unsigned source_bits : {16U, 32U, 64U}) {
		const unsigned source_bytes = source_bits / 8U;
		const unsigned destination_bits = source_bits / 2U;
		const unsigned destination_bytes = source_bytes / 2U;
		for (unsigned shift = 1; shift <= destination_bits; ++shift)
			for (const bool scalar : {false, true})
				for (const bool high : {false, true}) {
					if (scalar && high)
						continue;
					for (unsigned operation = 0; operation < 6; ++operation) {
						const bool rounding = (operation & 1U) != 0;
						const Mode mode = operation < 2U ? Mode::signed_unsigned :
							operation < 4U ? Mode::signed_signed :
							Mode::unsigned_unsigned;
						const unsigned opcode = mode == Mode::signed_unsigned ?
							(rounding ? 17U : 16U) : rounding ? 19U : 18U;
						const bool alternate = mode != Mode::signed_signed;
						auto initial = test::state(source_bits + shift + operation);
						initial.fpsr = 0x84U;
						initial.vectors[31].bytes.fill(std::byte{0xff});
						if (scalar)
							for (unsigned byte = 0; byte < source_bytes; ++byte)
								initial.vectors[31].bytes[byte] =
									static_cast<std::byte>(byte + 1U);
						const auto old_source = initial.vectors[31].bytes;
						const auto old_destination = initial.vectors[30].bytes;
						const auto encoded_instruction = instruction(scalar, high,
							alternate, opcode, source_bits - shift, 30, 31);
						const auto result = harness.run_interpreter(
							encoded_instruction, initial);
						std::array<std::byte, 16> expected{};
						if (high)
							std::copy_n(old_destination.begin(), 8, expected.begin());
						const unsigned source_limit = scalar ? source_bytes : 16U;
						unsigned output = high ? 8U : 0U;
						bool saturated = false;
						const auto unsigned_max =
							(static_cast<unsigned __int128>(1) << destination_bits) - 1U;
						const auto signed_min = -(
							static_cast<__int128>(1) << (destination_bits - 1U));
						const auto signed_max =
							(static_cast<__int128>(1) << (destination_bits - 1U)) - 1;
						for (unsigned input = 0; input < source_limit;
						     input += source_bytes, output += destination_bytes) {
							const auto raw = read_lane(
								old_source, input, source_bytes);
							std::uint64_t value;
							if (mode == Mode::unsigned_unsigned) {
								auto shifted = static_cast<unsigned __int128>(raw);
								if (rounding)
									shifted += static_cast<unsigned __int128>(1) <<
										(shift - 1U);
								shifted >>= shift;
								saturated |= shifted > unsigned_max;
								value = static_cast<std::uint64_t>(
									shifted > unsigned_max ? unsigned_max : shifted);
							} else {
								auto shifted = sign_extend(raw, source_bits);
								if (rounding)
									shifted += static_cast<__int128>(1) <<
										(shift - 1U);
								shifted >>= shift;
								if (mode == Mode::signed_unsigned) {
									saturated |= shifted < 0 ||
										static_cast<unsigned __int128>(shifted) > unsigned_max;
									value = shifted < 0 ? 0 : static_cast<std::uint64_t>(
										static_cast<unsigned __int128>(shifted) > unsigned_max ?
										unsigned_max : static_cast<unsigned __int128>(shifted));
								} else {
									saturated |= shifted < signed_min || shifted > signed_max;
									value = static_cast<std::uint64_t>(shifted < signed_min ?
										signed_min : shifted > signed_max ? signed_max : shifted);
								}
							}
							write_lane(expected, output, destination_bytes, value);
						}
						test::require(result.vectors[30].bytes == expected);
						test::require(result.fpsr ==
							(initial.fpsr | (saturated ? 1U << 27 : 0U)));
						harness.require_differential(
							encoded_instruction, initial);
					}
				}
	}
}

} // namespace

int main()
{
	every_operation_width_shift_part_and_scalar_isolation();
	return 0;
}
