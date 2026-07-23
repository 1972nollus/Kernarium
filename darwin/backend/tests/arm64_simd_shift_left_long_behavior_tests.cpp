// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t instruction(bool high,
	bool unsigned_source, unsigned encoded, unsigned destination,
	unsigned source)
{
	return 0x0f00'a400U | (high ? 0x4000'0000U : 0U) |
		(unsigned_source ? 0x2000'0000U : 0U) |
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

void every_size_shift_half_signedness_and_alias()
{
	test::Harness harness;
	for (unsigned source_bits : {8U, 16U, 32U}) {
		const unsigned source_bytes = source_bits / 8U;
		const unsigned destination_bytes = source_bytes * 2U;
		for (unsigned shift = 0; shift < source_bits; ++shift)
			for (const bool high : {false, true})
				for (const bool unsigned_source : {false, true})
					for (const bool alias : {false, true}) {
						const unsigned destination = alias ? 27U : 26U;
						const unsigned source = 27U;
						auto initial = test::state(source_bits + shift);
						const auto old_source = initial.vectors[source].bytes;
						const auto encoded_instruction = instruction(high,
							unsigned_source, source_bits + shift,
							destination, source);
						const auto result = harness.run_interpreter(
							encoded_instruction, initial);
						std::array<std::byte, 16> expected{};
						for (unsigned input = high ? 8U : 0U, output = 0;
						     output < 16U;
						     input += source_bytes, output += destination_bytes) {
							const auto raw = read_lane(
								old_source, input, source_bytes);
							const __int128 value = unsigned_source ? raw :
								sign_extend(raw, source_bits);
							write_lane(expected, output, destination_bytes,
								static_cast<std::uint64_t>(value *
									(static_cast<__int128>(1) << shift)));
						}
						test::require(result.vectors[destination].bytes == expected);
						harness.require_differential(
							encoded_instruction, initial);
					}
	}
}

} // namespace

int main()
{
	/* shift==0 covers the SXTL/SXTL2 and UXTL/UXTL2 aliases. */
	every_size_shift_half_signedness_and_alias();
	return 0;
}
