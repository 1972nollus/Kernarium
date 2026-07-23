// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <algorithm>

namespace test = arm64_simd_test;

namespace {

enum class Mode { signed_signed, signed_unsigned, unsigned_unsigned };

[[nodiscard]] constexpr std::uint32_t instruction(bool scalar, bool q,
	Mode mode, unsigned encoded, unsigned destination,
	unsigned source)
{
	const bool signed_unsigned = mode == Mode::signed_unsigned;
	return (scalar ? 0x5f00'0400U : 0x0f00'0400U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		(mode != Mode::signed_signed ? 0x2000'0000U : 0U) |
		((signed_unsigned ? 12U : 14U) << 11) |
		((encoded & 127U) << 16) | ((source & 31U) << 5) |
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

void every_shape_shift_signedness_saturation_and_scalar_isolation()
{
	test::Harness harness;
	for (const bool scalar : {false, true})
		for (const bool q : {false, true}) {
			if (scalar && q)
				continue;
			for (unsigned lane_bits : {8U, 16U, 32U, 64U}) {
				if (!scalar && !q && lane_bits == 64U)
					continue;
				const unsigned lane_bytes = lane_bits / 8U;
				const unsigned vector_bytes = scalar ? lane_bytes : q ? 16U : 8U;
				for (unsigned shift = 0; shift < lane_bits; ++shift)
					for (const auto mode : {Mode::signed_signed,
					     Mode::signed_unsigned, Mode::unsigned_unsigned})
						for (const bool extreme : {false, true}) {
							auto initial = test::state(lane_bits + shift + extreme);
							initial.fpsr = 0x15U;
							initial.vectors[29].bytes.fill(std::byte{0xff});
							if (!extreme)
								for (unsigned byte = 0; byte < lane_bytes; ++byte)
									initial.vectors[29].bytes[byte] =
										static_cast<std::byte>(byte + 1U);
							const auto old_source = initial.vectors[29].bytes;
							const auto encoded_instruction = instruction(scalar, q,
								mode, lane_bits + shift, 28, 29);
							const auto result = harness.run_interpreter(
								encoded_instruction, initial);
							std::array<std::byte, 16> expected{};
							bool saturated = false;
							const auto unsigned_max = lane_bits == 64U ?
								static_cast<unsigned __int128>(~std::uint64_t{0}) :
								(static_cast<unsigned __int128>(1) << lane_bits) - 1U;
							const auto signed_min = -(
								static_cast<__int128>(1) << (lane_bits - 1U));
							const auto signed_max =
								(static_cast<__int128>(1) << (lane_bits - 1U)) - 1;
							for (unsigned offset = 0; offset < vector_bytes;
							     offset += lane_bytes) {
								const auto raw = read_lane(
									old_source, offset, lane_bytes);
								std::uint64_t value;
								if (mode == Mode::unsigned_unsigned) {
									const auto shifted =
										static_cast<unsigned __int128>(raw) << shift;
									saturated |= shifted > unsigned_max;
									value = static_cast<std::uint64_t>(
										shifted > unsigned_max ? unsigned_max : shifted);
								} else {
									const auto shifted = sign_extend(raw, lane_bits) *
										(static_cast<__int128>(1) << shift);
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
								write_lane(expected, offset, lane_bytes, value);
							}
							test::require(result.vectors[28].bytes == expected);
							test::require(result.fpsr ==
								(initial.fpsr | (saturated ? 1U << 27 : 0U)));
							harness.require_differential(
								encoded_instruction, initial);
						}
			}
		}
}

void low_vectors_do_not_saturate_from_inactive_upper_lanes()
{
	test::Harness harness;
	for (const auto mode : {Mode::signed_unsigned, Mode::unsigned_unsigned}) {
		auto initial = test::state(static_cast<unsigned>(mode) + 91U);
		initial.fpsr = 0x15U;
		initial.vectors[29].bytes.fill(std::byte{0xff});
		std::fill_n(initial.vectors[29].bytes.begin(), 8, std::byte{0});
		const auto encoded_instruction = instruction(false, false, mode,
			9U, 28U, 29U);
		const auto interpreted = harness.run_interpreter(
			encoded_instruction, initial);
		test::require(interpreted.fpsr == initial.fpsr);
		harness.require_differential(encoded_instruction, initial);
	}
}

} // namespace

int main()
{
	every_shape_shift_signedness_saturation_and_scalar_isolation();
	low_vectors_do_not_saturate_from_inactive_upper_lanes();
	return 0;
}
