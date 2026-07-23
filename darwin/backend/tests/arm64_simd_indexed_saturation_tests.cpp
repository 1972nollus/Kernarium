// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_indexed_test_support.hpp"

#include <array>

namespace test = arm64_simd_indexed_test;
namespace base = arm64_simd_test;

namespace {

struct Registers final {
	unsigned destination;
	unsigned source;
	unsigned element;
};

void require_oracle(std::uint32_t encoded, bool unsigned_group, bool scalar,
	bool q, unsigned size, unsigned operation, unsigned destination,
	unsigned source, unsigned element_register, unsigned element_index,
	const uml::backend::arm64::CpuState &initial,
	base::Harness &harness)
{
	const bool long_result = !unsigned_group &&
		(operation == 3U || operation == 7U || operation == 11U);
	const bool rounding_accumulate = unsigned_group;
	const unsigned narrow_bytes = 1U << size;
	const unsigned narrow_bits = narrow_bytes * 8U;
	const unsigned result_bytes = long_result ? narrow_bytes * 2U : narrow_bytes;
	const unsigned result_bits = result_bytes * 8U;
	const unsigned vector_bytes = scalar ? narrow_bytes : q ? 16U : 8U;
	const unsigned lanes = scalar ? 1U : long_result ?
		16U / result_bytes : vector_bytes / narrow_bytes;
	const unsigned source_base = long_result && !scalar && q ? 8U : 0U;
	const auto old_destination = initial.vectors[destination].bytes;
	const auto old_source = initial.vectors[source].bytes;
	const auto old_element = initial.vectors[element_register].bytes;
	const auto raw_multiplier = test::read_lane(old_element,
		element_index * narrow_bytes, narrow_bytes);
	const __int128 multiplier =
		test::signed_value(raw_multiplier, narrow_bits);
	std::array<std::byte, 16> expected{};
	bool saturated = false;
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const auto raw_left = test::read_lane(old_source,
			source_base + lane * narrow_bytes, narrow_bytes);
		const __int128 doubled_product =
			test::signed_value(raw_left, narrow_bits) * multiplier * 2;
		std::uint64_t value;
		if (long_result) {
			value = test::saturate_signed(
				doubled_product, result_bits, saturated);
			if (operation == 3U || operation == 7U) {
				const auto raw_accumulator = test::read_lane(old_destination,
					lane * result_bytes, result_bytes);
				const __int128 accumulator =
					test::signed_value(raw_accumulator, result_bits);
				const __int128 product = test::signed_value(value, result_bits);
				value = test::saturate_signed(accumulator +
					(operation == 3U ? product : -product),
					result_bits, saturated);
			}
		} else {
			__int128 combined = doubled_product;
			if (rounding_accumulate) {
				const auto raw_accumulator = test::read_lane(old_destination,
					lane * narrow_bytes, narrow_bytes);
				combined = test::signed_value(raw_accumulator, narrow_bits) *
					(static_cast<__int128>(1) << narrow_bits) +
					(operation == 13U ? doubled_product : -doubled_product);
			}
			if (operation == 13U || rounding_accumulate)
				combined += static_cast<__int128>(1) << (narrow_bits - 1U);
			value = test::saturate_signed(
				combined >> narrow_bits, narrow_bits, saturated);
		}
		test::write_lane(expected, lane * result_bytes, result_bytes, value);
	}
	auto expected_state = initial;
	expected_state.pc = base::pc + 4U;
	expected_state.vectors[destination].bytes = expected;
	if (saturated)
		expected_state.fpsr |= 1U << 27;
	const auto interpreted = harness.run_interpreter(encoded, initial);
	base::require_architecture_equal(expected_state, interpreted);
	harness.require_differential(encoded, initial);
}

void prepare_extreme(uml::backend::arm64::CpuState &initial,
	bool unsigned_group, unsigned size, unsigned operation,
	unsigned destination, unsigned source, unsigned element)
{
	const unsigned narrow_bytes = 1U << size;
	const unsigned narrow_bits = narrow_bytes * 8U;
	const bool long_result = !unsigned_group &&
		(operation == 3U || operation == 7U || operation == 11U);
	const unsigned result_bits = long_result ? narrow_bits * 2U : narrow_bits;
	const unsigned result_bytes = result_bits / 8U;
	const std::uint64_t minimum = std::uint64_t{1} << (narrow_bits - 1U);
	const std::uint64_t result_minimum =
		std::uint64_t{1} << (result_bits - 1U);
	const std::uint64_t result_maximum = result_minimum - 1U;
	test::set_all_lanes(initial.vectors[source].bytes, narrow_bytes, minimum);
	test::set_all_lanes(initial.vectors[element].bytes, narrow_bytes, minimum);
	const bool subtract = operation == 7U || operation == 15U;
	test::set_all_lanes(initial.vectors[destination].bytes, result_bytes,
		subtract ? result_minimum : result_maximum);
}

void every_operation_shape_index_rounding_and_saturation()
{
	base::Harness harness;
	struct Operation final { bool unsigned_group; unsigned opcode; };
	constexpr std::array operations{
		Operation{false, 3U}, Operation{false, 7U}, Operation{false, 11U},
		Operation{false, 12U}, Operation{false, 13U},
		Operation{true, 13U}, Operation{true, 15U}};
	for (const auto operation : operations)
		for (const bool scalar : {false, true})
			for (unsigned size : {1U, 2U}) {
				const unsigned index_count = size == 1U ? 8U : 4U;
				for (const bool q : {false, true}) {
					if (scalar && q)
						continue;
					for (unsigned index = 0; index < index_count; ++index)
						for (const bool extreme : {false, true}) {
							auto initial = base::state(operation.opcode * 17U +
								size * 11U + index + scalar + q + extreme);
							initial.fpsr = 0x84U;
							const unsigned element = size == 1U ? 15U : 19U;
							if (extreme)
								prepare_extreme(initial, operation.unsigned_group,
									size, operation.opcode, 28U, 29U, element);
							const auto encoded = test::instruction(scalar, q,
								operation.unsigned_group, size, operation.opcode,
								28U, 29U, element, index);
							require_oracle(encoded, operation.unsigned_group,
								scalar, q, size, operation.opcode, 28U, 29U,
								element, index, initial, harness);
						}
				}
			}
}

void aliases_and_cumulative_qc()
{
	base::Harness harness;
	struct Operation final { bool unsigned_group; unsigned opcode; };
	constexpr std::array operations{
		Operation{false, 3U}, Operation{false, 7U}, Operation{false, 11U},
		Operation{false, 12U}, Operation{false, 13U},
		Operation{true, 13U}, Operation{true, 15U}};
	constexpr std::array registers{
		Registers{1U, 1U, 2U}, Registers{2U, 1U, 2U},
		Registers{1U, 2U, 2U}, Registers{3U, 3U, 3U}};
	for (const auto operation : operations)
		for (const auto regs : registers) {
			auto initial = base::state(operation.opcode + regs.destination * 9U);
			initial.fpsr = 0x0800'0084U;
			const auto encoded = test::instruction(false, true,
				operation.unsigned_group, 1U, operation.opcode,
				regs.destination, regs.source, regs.element, 7U);
			require_oracle(encoded, operation.unsigned_group, false, true, 1U,
				operation.opcode, regs.destination, regs.source, regs.element,
				7U, initial, harness);
		}
}

} // namespace

int main()
{
	every_operation_shape_index_rounding_and_saturation();
	aliases_and_cumulative_qc();
	return 0;
}
