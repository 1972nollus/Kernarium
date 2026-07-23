// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

#include <span>

namespace test = arm64_simd_test;

namespace {

void fill(test::arm64::VectorRegister &vector, std::uint64_t value,
	unsigned size)
{
	const std::size_t lane_size = std::size_t{1} << size;
	for (std::size_t offset = 0; offset < vector.bytes.size();
	     offset += lane_size) {
		for (std::size_t byte = 0; byte < lane_size; ++byte)
			vector.bytes[offset + byte] = static_cast<std::byte>(value >>
				(byte * 8));
	}
}

void qc_is_cumulative_for_every_saturating_shape()
{
	test::Harness harness;
	constexpr unsigned qc = 1U << 27;
	for (const bool scalar : {false, true}) {
		for (const bool q : {false, true}) {
			if (scalar && q)
				continue;
			for (const bool unsigned_group : {false, true}) {
				for (unsigned size = 0; size < 4; ++size) {
					for (const unsigned opcode : {1U, 5U, 9U, 11U, 22U}) {
						if (!test::valid_integer_three_same(scalar, q,
						    unsigned_group, size, opcode))
							continue;
						auto initial = test::state(size + opcode);
						const unsigned bits = 8U << size;
						const std::uint64_t maximum = bits == 64 ?
							~std::uint64_t{} : (std::uint64_t{1} << bits) - 1;
						const std::uint64_t signed_maximum = maximum >> 1;
						const std::uint64_t signed_minimum =
							std::uint64_t{1} << (bits - 1);
						if (opcode == 1) {
							fill(initial.vectors[1], unsigned_group ? maximum :
								signed_maximum, size);
							fill(initial.vectors[2], 1, size);
						} else if (opcode == 5) {
							fill(initial.vectors[1], unsigned_group ? 0 :
								signed_minimum, size);
							fill(initial.vectors[2], 1, size);
						} else if (opcode == 22) {
							fill(initial.vectors[1], signed_minimum, size);
							fill(initial.vectors[2], signed_minimum, size);
						} else {
							fill(initial.vectors[1], unsigned_group ? maximum :
								signed_maximum, size);
							fill(initial.vectors[2], 1, size);
						}
						initial.fpsr = 1U;
						const auto instruction = test::three_same(scalar, q,
							unsigned_group, size, opcode, 0, 1, 2);
						const auto saturated = harness.run_interpreter(
							instruction, initial);
						test::require((saturated.fpsr & qc) != 0);
						test::require((saturated.fpsr & 1U) != 0);
						harness.require_differential(instruction, initial);

						/* A prior QC flag must survive a non-saturating input. */
						fill(initial.vectors[1], 0, size);
						fill(initial.vectors[2], 0, size);
						initial.fpsr = qc | 1U;
						const auto cumulative = harness.run_interpreter(
							instruction, initial);
						test::require((cumulative.fpsr & (qc | 1U)) ==
							(qc | 1U));
						harness.require_differential(instruction, initial);
					}
				}
			}
		}
	}
}

void signed_left_shift_avoids_undefined_host_arithmetic()
{
	test::Harness harness;
	for (unsigned size = 0; size < 4; ++size) {
		const bool q = size == 3;
		auto initial = test::state(size);
		const unsigned bits = 8U << size;
		fill(initial.vectors[1], std::uint64_t{1} << (bits - 1), size);
		fill(initial.vectors[2], bits - 1, size);
		for (const unsigned opcode : {9U, 11U})
			harness.require_differential(test::three_same(false, q, false,
				size, opcode, 0, 1, 2), initial);
	}
}

} // namespace

int main()
{
	qc_is_cumulative_for_every_saturating_shape();
	signed_left_shift_avoids_undefined_host_arithmetic();
	return 0;
}
