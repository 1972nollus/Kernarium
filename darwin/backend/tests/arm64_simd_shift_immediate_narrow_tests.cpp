// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t instruction(bool high, bool rounding,
	unsigned encoded, unsigned destination, unsigned source)
{
	return 0x0f00'8400U | (high ? 0x4000'0000U : 0U) |
		(rounding ? 0x800U : 0U) | ((encoded & 127U) << 16) |
		((source & 31U) << 5) | (destination & 31U);
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

void every_width_shift_half_rounding_alias_and_overflow()
{
	test::Harness harness;
	for (unsigned source_bits : {16U, 32U, 64U}) {
		const unsigned source_bytes = source_bits / 8U;
		const unsigned destination_bytes = source_bytes / 2U;
		for (unsigned shift = 1; shift <= source_bits / 2U; ++shift)
			for (const bool high : {false, true})
				for (const bool rounding : {false, true})
					for (const bool alias : {false, true})
						for (const bool maximum_input : {false, true}) {
							const unsigned destination = alias ? 24U : 23U;
							const unsigned source = 24U;
							auto initial = test::state(source_bits + shift);
							if (maximum_input)
								initial.vectors[source].bytes.fill(std::byte{0xff});
							const auto old_source = initial.vectors[source].bytes;
							const auto old_destination =
								initial.vectors[destination].bytes;
							const auto encoded_instruction = instruction(high,
								rounding, source_bits - shift, destination, source);
							const auto result = harness.run_interpreter(
								encoded_instruction, initial);
							std::array<std::byte, 16> expected{};
							if (high)
								std::copy_n(old_destination.begin(), 8,
									expected.begin());
							for (unsigned input = 0, output = high ? 8U : 0U;
							     input < 16U;
							     input += source_bytes, output += destination_bytes) {
								auto value = static_cast<unsigned __int128>(
									read_lane(old_source, input, source_bytes));
								if (rounding)
									value += static_cast<unsigned __int128>(1) <<
										(shift - 1U);
								write_lane(expected, output, destination_bytes,
									static_cast<std::uint64_t>(value >> shift));
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
	every_width_shift_half_rounding_alias_and_overflow();
	return 0;
}
