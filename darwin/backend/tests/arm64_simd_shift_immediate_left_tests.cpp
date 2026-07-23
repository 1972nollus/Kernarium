// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <bit>

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t instruction(bool scalar, bool q,
	bool insert, unsigned encoded, unsigned destination, unsigned source)
{
	return (scalar ? 0x5f00'5400U : 0x0f00'5400U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		(insert ? 0x2000'0000U : 0U) | ((encoded & 127U) << 16) |
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

void every_shape_shift_insert_and_alias()
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
				const unsigned lane_bytes = lane_bits / 8U;
				const unsigned vector_bytes = scalar ? 8U : q ? 16U : 8U;
				const std::uint64_t mask = lane_bits == 64U ?
					~std::uint64_t{0} : (std::uint64_t{1} << lane_bits) - 1U;
				for (unsigned shift = 0; shift < lane_bits; ++shift)
					for (const bool insert : {false, true})
						for (const bool alias : {false, true}) {
							const unsigned destination = alias ? 22U : 21U;
							const unsigned source = 22U;
							auto initial = test::state(lane_bits + shift);
							const auto old_source = initial.vectors[source].bytes;
							const auto old_destination =
								initial.vectors[destination].bytes;
							const auto encoded_instruction = instruction(scalar, q,
								insert, lane_bits + shift, destination, source);
							const auto result = harness.run_interpreter(
								encoded_instruction, initial);
							std::array<std::byte, 16> expected{};
							for (unsigned offset = 0; offset < vector_bytes;
							     offset += lane_bytes) {
								const auto source_value =
									read_lane(old_source, offset, lane_bytes);
								auto value = (source_value << shift) & mask;
								if (insert) {
									const auto low_mask = shift == 0U ? 0U :
										(std::uint64_t{1} << shift) - 1U;
									value |= read_lane(old_destination, offset,
										lane_bytes) & low_mask;
								}
								write_lane(expected, offset, lane_bytes, value);
							}
							test::require(result.vectors[destination].bytes == expected);
							harness.require_differential(
								encoded_instruction, initial);
						}
			}
		}
}

} // namespace

int main()
{
	every_shape_shift_insert_and_alias();
	return 0;
}
