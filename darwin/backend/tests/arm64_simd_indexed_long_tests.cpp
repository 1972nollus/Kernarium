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

void require_oracle(std::uint32_t encoded, bool unsigned_group,
	unsigned size, bool high, unsigned operation, unsigned destination,
	unsigned source, unsigned element_register, unsigned element_index,
	const uml::backend::arm64::CpuState &initial,
	base::Harness &harness)
{
	const unsigned narrow_bytes = 1U << size;
	const unsigned narrow_bits = narrow_bytes * 8U;
	const unsigned wide_bytes = narrow_bytes * 2U;
	const unsigned wide_bits = narrow_bits * 2U;
	const auto old_destination = initial.vectors[destination].bytes;
	const auto old_source = initial.vectors[source].bytes;
	const auto old_element = initial.vectors[element_register].bytes;
	const auto raw_multiplier = test::read_lane(old_element,
		element_index * narrow_bytes, narrow_bytes);
	const __int128 signed_multiplier =
		test::signed_value(raw_multiplier, narrow_bits);
	std::array<std::byte, 16> expected{};
	const unsigned source_base = high ? 8U : 0U;
	for (unsigned lane = 0; lane < 16U / wide_bytes; ++lane) {
		const auto raw_left = test::read_lane(old_source,
			source_base + lane * narrow_bytes, narrow_bytes);
		const __int128 product = unsigned_group ?
			static_cast<__int128>(static_cast<unsigned __int128>(raw_left) *
				raw_multiplier) :
			test::signed_value(raw_left, narrow_bits) * signed_multiplier;
		const auto accumulator = test::read_lane(old_destination,
			lane * wide_bytes, wide_bytes);
		unsigned __int128 value = static_cast<unsigned __int128>(product);
		if (operation == 2U)
			value += accumulator;
		else if (operation == 6U)
			value = static_cast<unsigned __int128>(accumulator) - value;
		test::write_lane(expected, lane * wide_bytes, wide_bytes,
			static_cast<std::uint64_t>(value & test::mask(wide_bits)));
	}
	auto expected_state = initial;
	expected_state.pc = base::pc + 4U;
	expected_state.vectors[destination].bytes = expected;
	const auto interpreted = harness.run_interpreter(encoded, initial);
	base::require_architecture_equal(expected_state, interpreted);
	harness.require_differential(encoded, initial);
}

void every_signedness_operation_half_index_and_alias()
{
	base::Harness harness;
	for (const bool unsigned_group : {false, true})
		for (const unsigned operation : {2U, 6U, 10U})
			for (unsigned size : {1U, 2U}) {
				const unsigned index_count = size == 1U ? 8U : 4U;
				for (const bool high : {false, true})
					for (unsigned index = 0; index < index_count; ++index) {
						const std::array registers{
							Registers{28U, 29U, size == 1U ? 15U : 19U},
							Registers{1U, 1U, 2U},
							Registers{2U, 1U, 2U},
							Registers{1U, 2U, 2U},
							Registers{3U, 3U, 3U}};
						for (const auto regs : registers) {
							auto initial = base::state(operation * 13U +
								size * 5U + index + high + unsigned_group);
							initial.fpsr = 0x84U;
							const auto encoded = test::instruction(false, high,
								unsigned_group, size, operation,
								regs.destination, regs.source, regs.element, index);
							require_oracle(encoded, unsigned_group, size, high,
								operation, regs.destination, regs.source, regs.element,
								index, initial, harness);
						}
					}
			}
}

} // namespace

int main()
{
	every_signedness_operation_half_index_and_alias();
	return 0;
}
