// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_facmp_test_support.hpp"

namespace test = arm64_simd_facmp_test;
namespace base = arm64_simd_test;

namespace {

void every_nan_kind_source_and_operation_signals_invalid()
{
	base::Harness harness;
	for (const auto shape : test::shapes)
		for (const auto operation : {test::Operation::greater_equal,
		     test::Operation::greater})
			for (const bool quiet : {false, true})
				for (const bool nan_on_left : {false, true}) {
					auto initial = base::state(shape.width + quiet + nan_on_left);
					initial.fpsr = 0x0800'0090U;
					const unsigned bytes = shape.width / 8U;
					const unsigned lanes = shape.vector_bytes / bytes;
					std::array<bool, 8> matches{};
					for (unsigned lane = 0; lane < lanes; ++lane) {
						const auto nan = quiet ? test::scalar::quiet_nan(bytes) :
							test::scalar::signaling_nan(bytes);
						test::compare::write_lane(initial.vectors[29].bytes,
							shape, lane, nan_on_left ? nan :
								test::scalar::positive_one(bytes));
						test::compare::write_lane(initial.vectors[30].bytes,
							shape, lane, nan_on_left ?
								test::scalar::positive_one(bytes) : nan);
					}
					test::require_result(test::instruction(operation, shape,
						28U, 29U, 30U), shape, operation, 28U, 30U,
						std::span{matches}.first(lanes), 1U, initial, harness);
				}
}

} // namespace

int main()
{
	every_nan_kind_source_and_operation_signals_invalid();
	return 0;
}
