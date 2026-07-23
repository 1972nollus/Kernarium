// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_cvtfi_test_support.hpp"

namespace test = arm64_simd_cvtfi_test;
namespace base = arm64_simd_test;

int main()
{
	base::Harness harness;
	for (const unsigned integer_width : {32U, 64U})
		for (const unsigned destination_width : {16U, 32U, 64U})
			for (const bool unsigned_source : {false, true})
				for (const unsigned fractional_bits : {1U, 8U}) {
					auto initial = base::state(integer_width +
						destination_width + fractional_bits);
					initial.registers[29] =
						std::uint64_t{3} << (fractional_bits - 1U);
					const std::array output{test::fp::floating_bits(
						destination_width, false, false)};
					test::require_vector(test::gpr_fixed_instruction(
						unsigned_source, integer_width, destination_width,
						fractional_bits, 28U, 29U), destination_width,
						destination_width / 8U, true, 28U, output, 0U,
						initial, harness);
				}

	for (const auto shape : test::shapes) {
		auto initial = base::state(100U + shape.width);
		const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
		std::array<std::uint64_t, 8> output{};
		for (unsigned lane = 0; lane < lanes; ++lane) {
			test::set_lane(initial, 29U, shape.width, lane, 384U);
			output[lane] = test::fp::floating_bits(
				shape.width, false, false);
		}
		test::require_vector(test::simd_fixed_instruction(false, shape,
			8U, 28U, 29U), shape.width, shape.vector_bytes, shape.scalar,
			28U, std::span{output}.first(lanes), 0U, initial, harness);
	}
	return 0;
}
