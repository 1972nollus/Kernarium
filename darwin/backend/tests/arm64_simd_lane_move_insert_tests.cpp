// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <bit>

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t insert_general(unsigned immediate,
	unsigned destination, unsigned source)
{
	return 0x4e00'1c00U | ((immediate & 31U) << 16) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t insert_element(unsigned immediate,
	unsigned imm4, unsigned destination, unsigned source)
{
	return 0x6e00'0400U | ((immediate & 31U) << 16) |
		((imm4 & 15U) << 11) | ((source & 31U) << 5) |
		(destination & 31U);
}

void every_general_lane_and_zero_register()
{
	test::Harness harness;
	for (unsigned immediate = 0; immediate < 32; ++immediate) {
		if ((immediate & 15U) == 0)
			continue;
		const unsigned shift = std::countr_zero(immediate);
		const unsigned bytes = 1U << shift;
		const unsigned destination_offset =
			(immediate >> (shift + 1U)) << shift;
		for (const unsigned source : {12U, 31U}) {
			auto initial = test::state(immediate + source);
			initial.registers[12] = 0x8172'6354'4536'2718ULL;
			const auto old_destination = initial.vectors[5].bytes;
			const auto instruction = insert_general(immediate, 5, source);
			const auto result = harness.run_interpreter(instruction, initial);
			const std::uint64_t value = source == 31U ? 0 :
				initial.registers[source];
			for (unsigned byte = 0; byte < 16; ++byte) {
				const bool replaced = byte >= destination_offset &&
					byte < destination_offset + bytes;
				const auto expected = replaced ? static_cast<std::byte>(
					value >> ((byte - destination_offset) * 8U)) :
					old_destination[byte];
				test::require(result.vectors[5].bytes[byte] == expected);
			}
			harness.require_differential(instruction, initial);
		}
	}
}

void every_element_lane_ignored_bit_and_alias()
{
	test::Harness harness;
	for (unsigned immediate = 0; immediate < 32; ++immediate) {
		if ((immediate & 15U) == 0)
			continue;
		const unsigned shift = std::countr_zero(immediate);
		const unsigned bytes = 1U << shift;
		const unsigned destination_offset =
			(immediate >> (shift + 1U)) << shift;
		for (unsigned imm4 = 0; imm4 < 16; ++imm4) {
			const unsigned source_offset = (imm4 >> shift) << shift;
			for (const bool alias : {false, true}) {
				auto initial = test::state(immediate * 17U + imm4);
				const unsigned destination = 14U;
				const unsigned source = alias ? destination : 15U;
				const auto old_destination = initial.vectors[destination].bytes;
				const auto old_source = initial.vectors[source].bytes;
				const auto instruction = insert_element(immediate, imm4,
					destination, source);
				const auto result = harness.run_interpreter(
					instruction, initial);
				for (unsigned byte = 0; byte < 16; ++byte) {
					const bool replaced = byte >= destination_offset &&
						byte < destination_offset + bytes;
					const auto expected = replaced ? old_source[
						source_offset + byte - destination_offset] :
						old_destination[byte];
					test::require(result.vectors[destination].bytes[byte] ==
						expected);
				}
				harness.require_differential(instruction, initial);
			}
		}
	}
}

} // namespace

int main()
{
	/* These are also the MOV (from general) and MOV (element) aliases. */
	every_general_lane_and_zero_register();
	every_element_lane_ignored_bit_and_alias();
	return 0;
}
