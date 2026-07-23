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

void require_oracle(std::uint32_t encoded, unsigned size, bool q,
	unsigned operation, unsigned destination, unsigned source,
	unsigned element_register, unsigned element_index,
	const uml::backend::arm64::CpuState &initial,
	base::Harness &harness)
{
	const unsigned lane_bytes = 1U << size;
	const unsigned lane_bits = lane_bytes * 8U;
	const unsigned vector_bytes = q ? 16U : 8U;
	const auto old_destination = initial.vectors[destination].bytes;
	const auto old_source = initial.vectors[source].bytes;
	const auto old_element = initial.vectors[element_register].bytes;
	const auto multiplier = test::read_lane(old_element,
		element_index * lane_bytes, lane_bytes);
	std::array<std::byte, 16> expected{};
	for (unsigned offset = 0; offset < vector_bytes; offset += lane_bytes) {
		const auto left = test::read_lane(old_source, offset, lane_bytes);
		const auto accumulator = test::read_lane(
			old_destination, offset, lane_bytes);
		unsigned __int128 value =
			static_cast<unsigned __int128>(left) * multiplier;
		if (operation == 0U)
			value += accumulator;
		else if (operation == 4U)
			value = static_cast<unsigned __int128>(accumulator) - value;
		test::write_lane(expected, offset, lane_bytes,
			static_cast<std::uint64_t>(value & test::mask(lane_bits)));
	}
	auto expected_state = initial;
	expected_state.pc = base::pc + 4U;
	expected_state.vectors[destination].bytes = expected;
	const auto interpreted = harness.run_interpreter(encoded, initial);
	base::require_architecture_equal(expected_state, interpreted);
	harness.require_differential(encoded, initial);
}

void every_operation_shape_index_and_alias()
{
	base::Harness harness;
	for (const unsigned operation : {0U, 4U, 8U}) {
		const bool unsigned_group = operation != 8U;
		for (unsigned size : {1U, 2U}) {
			const unsigned index_count = size == 1U ? 8U : 4U;
			for (const bool q : {false, true})
				for (unsigned index = 0; index < index_count; ++index) {
					const std::array registers{
						Registers{28U, 29U, size == 1U ? 15U : 19U},
						Registers{1U, 1U, 2U},
						Registers{2U, 1U, 2U},
						Registers{1U, 2U, 2U},
						Registers{3U, 3U, 3U}};
					for (const auto regs : registers) {
						auto initial = base::state(
							operation * 19U + size * 7U + index + q);
						initial.fpsr = 0x84U;
						const auto encoded = test::instruction(false, q,
							unsigned_group, size, operation, regs.destination,
							regs.source, regs.element, index);
						require_oracle(encoded, size, q, operation,
							regs.destination, regs.source, regs.element, index,
							initial, harness);
					}
				}
		}
	}
}

} // namespace

int main()
{
	every_operation_shape_index_and_alias();
	return 0;
}
