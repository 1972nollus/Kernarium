// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_floating_compare_test_support.hpp"

namespace test = arm64_simd_floating_compare_test;
namespace base = arm64_simd_test;

namespace {

void afp_subnormal_matrix()
{
	base::Harness harness;
	struct Entry final {
		std::uint32_t fpcr;
		bool equal_zero;
		std::uint32_t exceptions;
	};
	const Entry entries[]{
		{0U, false, 0U},
		{1U << 24, true, 1U << 7},
		{1U, true, 0U},
		{(1U << 24) | 1U, true, 1U << 7},
		{2U, false, 1U << 7},
		{(1U << 24) | 2U, false, 1U << 7},
		{3U, true, 0U},
	};
	for (const auto shape : test::shapes) {
		if (shape.width == 16U)
			continue;
		for (const auto entry : entries) {
			auto initial = base::state(shape.width + entry.fpcr);
			initial.fpcr = entry.fpcr;
			initial.fpsr = 0x10U;
			const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
			std::array<bool, 8> matches{};
			for (unsigned lane = 0; lane < lanes; ++lane) {
				test::write_lane(initial.vectors[29].bytes, shape, lane, 1U);
				matches[lane] = entry.equal_zero;
			}
			test::require_result(test::zero_instruction(test::Operation::equal,
				shape, 28U, 29U), shape, test::Operation::equal, true,
				28U, 0U, std::span{matches}.first(lanes), entry.exceptions,
				initial, harness);
		}
	}
}

void half_fz16_and_every_rare_operation()
{
	base::Harness harness;
	for (const auto shape : test::shapes) {
		for (const auto operation : {test::Operation::equal,
		     test::Operation::greater_equal, test::Operation::greater,
		     test::Operation::less_equal, test::Operation::less}) {
			auto initial = base::state(shape.width + static_cast<unsigned>(operation));
			initial.fpcr = 3U;
			const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
			const bool match = operation == test::Operation::greater_equal ||
				operation == test::Operation::greater;
			std::array<bool, 8> matches{};
			for (unsigned lane = 0; lane < lanes; ++lane) {
				test::write_lane(initial.vectors[29].bytes, shape, lane,
					test::scalar::positive_one(shape.width / 8U));
				test::write_lane(initial.vectors[30].bytes, shape, lane, 0U);
				matches[lane] = match;
			}
			if (operation == test::Operation::less_equal ||
			    operation == test::Operation::less)
				test::require_result(test::zero_instruction(operation, shape,
					28U, 29U), shape, operation, true, 28U, 0U,
					std::span{matches}.first(lanes), 0U, initial, harness);
			else {
				test::require_result(test::register_instruction(operation, shape,
					28U, 29U, 30U), shape, operation, false, 28U, 30U,
					std::span{matches}.first(lanes), 0U, initial, harness);
			}
		}
	}

	for (const auto shape : {test::shapes[0], test::shapes[3], test::shapes[4]}) {
		auto initial = base::state(shape.vector_bytes);
		initial.fpcr = 1U << 19;
		const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
		std::array<bool, 8> matches{};
		for (unsigned lane = 0; lane < lanes; ++lane) {
			test::write_lane(initial.vectors[29].bytes, shape, lane, 1U);
			matches[lane] = true;
		}
		test::require_result(test::zero_instruction(test::Operation::equal,
			shape, 28U, 29U), shape, test::Operation::equal, true,
			28U, 0U, std::span{matches}.first(lanes), 0U, initial, harness);
	}
}

} // namespace

int main()
{
	afp_subnormal_matrix();
	half_fz16_and_every_rare_operation();
	return 0;
}
