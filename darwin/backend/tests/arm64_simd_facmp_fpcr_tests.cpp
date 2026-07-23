// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_facmp_test_support.hpp"

namespace test = arm64_simd_facmp_test;
namespace base = arm64_simd_test;

namespace {

void afp_subnormal_matrix()
{
	base::Harness harness;
	struct Entry final {
		std::uint32_t fpcr;
		bool greater;
		std::uint32_t exceptions;
	};
	const Entry entries[]{
		{0U, true, 0U},
		{1U << 24, false, 1U << 7},
		{1U, false, 0U},
		{(1U << 24) | 1U, false, 1U << 7},
		{2U, true, 1U << 7},
		{(1U << 24) | 2U, true, 1U << 7},
		{3U, false, 0U},
	};
	for (const auto shape : test::shapes) {
		if (shape.width == 16U)
			continue;
		for (const auto entry : entries)
			for (const auto operation : {test::Operation::greater_equal,
			     test::Operation::greater}) {
				auto initial = base::state(shape.width + entry.fpcr);
				initial.fpcr = entry.fpcr;
				initial.fpsr = 0x10U;
				const unsigned lanes = shape.vector_bytes / (shape.width / 8U);
				std::array<bool, 8> matches{};
				for (unsigned lane = 0; lane < lanes; ++lane) {
					test::compare::write_lane(initial.vectors[29].bytes,
						shape, lane, test::scalar::minimum_subnormal(
							shape.width / 8U, true));
					test::compare::write_lane(initial.vectors[30].bytes,
						shape, lane, 0U);
					matches[lane] = operation == test::Operation::greater_equal ||
						entry.greater;
				}
				test::require_result(test::instruction(operation, shape,
					28U, 29U, 30U), shape, operation, 28U, 30U,
					std::span{matches}.first(lanes), entry.exceptions,
					initial, harness);
			}
	}
}

void half_flush_to_zero()
{
	base::Harness harness;
	for (const auto shape : {test::shapes[0], test::shapes[3], test::shapes[4]})
		for (const auto operation : {test::Operation::greater_equal,
		     test::Operation::greater}) {
			auto initial = base::state(shape.vector_bytes + 90U);
			initial.fpcr = 1U << 19;
			const unsigned lanes = shape.vector_bytes / 2U;
			std::array<bool, 8> matches{};
			for (unsigned lane = 0; lane < lanes; ++lane) {
				test::compare::write_lane(initial.vectors[29].bytes,
					shape, lane, 0x8001U);
				test::compare::write_lane(initial.vectors[30].bytes,
					shape, lane, 0U);
				matches[lane] = operation == test::Operation::greater_equal;
			}
			test::require_result(test::instruction(operation, shape,
				28U, 29U, 30U), shape, operation, 28U, 30U,
				std::span{matches}.first(lanes), 0U, initial, harness);
		}
}

} // namespace

int main()
{
	afp_subnormal_matrix();
	half_flush_to_zero();
	return 0;
}
