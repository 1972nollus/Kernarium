// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <bit>

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t duplicate_element(bool scalar, bool q,
	unsigned immediate, unsigned destination, unsigned source)
{
	return (scalar ? 0x5e00'0400U : 0x0e00'0400U) |
		(!scalar && q ? 0x4000'0000U : 0U) |
		((immediate & 31U) << 16) | ((source & 31U) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t duplicate_general(bool q,
	unsigned immediate, unsigned destination, unsigned source)
{
	return 0x0e00'0c00U | (q ? 0x4000'0000U : 0U) |
		((immediate & 31U) << 16) | ((source & 31U) << 5) |
		(destination & 31U);
}

void every_element_size_lane_shape_and_alias()
{
	test::Harness harness;
	for (unsigned immediate = 0; immediate < 32; ++immediate) {
		if ((immediate & 15U) == 0)
			continue;
		const unsigned shift = std::countr_zero(immediate);
		const unsigned bytes = 1U << shift;
		const unsigned source_offset =
			(immediate >> (shift + 1U)) << shift;
		for (const bool scalar : {false, true})
			for (const bool q : {false, true}) {
				if (!scalar && !q && shift == 3U)
					continue;
				if (scalar && q)
					continue;
				const unsigned output_bytes = scalar ? bytes : q ? 16U : 8U;
				for (const bool alias : {false, true}) {
					auto initial = test::state(immediate + q * 37U);
					const unsigned destination = alias ? 7U : 6U;
					const unsigned source = 7U;
					const auto source_bytes = initial.vectors[source].bytes;
					const auto instruction = duplicate_element(scalar, q,
						immediate, destination, source);
					const auto result = harness.run_interpreter(
						instruction, initial);
					for (unsigned byte = 0; byte < output_bytes; ++byte)
						test::require(result.vectors[destination].bytes[byte] ==
							source_bytes[source_offset + byte % bytes]);
					for (unsigned byte = output_bytes; byte < 16; ++byte)
						test::require(result.vectors[destination].bytes[byte] ==
							std::byte{});
					harness.require_differential(instruction, initial);
				}
			}
	}
}

void every_general_size_shape_ignored_bit_and_zero_register()
{
	test::Harness harness;
	for (unsigned immediate = 0; immediate < 32; ++immediate) {
		if ((immediate & 15U) == 0)
			continue;
		const unsigned shift = std::countr_zero(immediate);
		const unsigned bytes = 1U << shift;
		for (const bool q : {false, true}) {
			if (!q && shift == 3U)
				continue;
			for (const unsigned source : {9U, 31U}) {
				auto initial = test::state(immediate + source);
				initial.registers[9] = 0xf1e2'd3c4'b5a6'9788ULL;
				const auto instruction = duplicate_general(
					q, immediate, 4, source);
				const auto result = harness.run_interpreter(
					instruction, initial);
				const std::uint64_t value = source == 31U ? 0 :
					initial.registers[source];
				const unsigned output_bytes = q ? 16U : 8U;
				for (unsigned byte = 0; byte < output_bytes; ++byte)
					test::require(result.vectors[4].bytes[byte] ==
						static_cast<std::byte>(value >>
							((byte % bytes) * 8U)));
				for (unsigned byte = output_bytes; byte < 16; ++byte)
					test::require(result.vectors[4].bytes[byte] == std::byte{});
				harness.require_differential(instruction, initial);
			}
		}
	}
}

} // namespace

int main()
{
	/* Scalar DUP is the MOV (scalar) alias encoding. */
	every_element_size_lane_shape_and_alias();
	every_general_size_shape_ignored_bit_and_zero_register();
	return 0;
}
