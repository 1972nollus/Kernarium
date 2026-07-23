// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_dot_test_support.hpp"

#include <array>

namespace test = arm64_simd_dot_test;
namespace indexed = arm64_simd_indexed_test;
namespace base = arm64_simd_test;

namespace {

void vector_sizes_and_scalar_holes()
{
	base::Harness harness;
	for (const auto mode : {test::Mode::signed_signed,
	     test::Mode::unsigned_unsigned, test::Mode::unsigned_signed})
		for (const bool q : {false, true})
			for (unsigned size = 0; size < 4U; ++size) {
				const auto encoded = test::vector_instruction(
					mode, q, size, 28U, 29U, 30U);
				if (size == 2U)
					harness.require_differential(encoded,
						base::state(size + q + static_cast<unsigned>(mode)));
				else
					harness.require_illegal(encoded,
						base::state(size + q + static_cast<unsigned>(mode)));
			}
	for (const bool unsigned_group : {false, true})
		harness.require_illegal(indexed::instruction(true, false,
			unsigned_group, 2U, 14U, 28U, 29U, 2U, 0U));
	for (const unsigned size : {0U, 2U})
		harness.require_illegal(indexed::instruction(true, false,
			false, size, 15U, 28U, 29U, 2U, 0U));
}

void indexed_size_allocation()
{
	base::Harness harness;
	for (const bool unsigned_group : {false, true})
		for (const bool q : {false, true})
			for (unsigned size = 0; size < 4U; ++size) {
				const auto encoded = indexed::instruction(false, q,
					unsigned_group, size, 14U, 28U, 29U, 19U, 0U);
				if (size == 2U)
					harness.require_differential(encoded,
						base::state(size + q + unsigned_group));
				else
					harness.require_illegal(encoded,
						base::state(size + q + unsigned_group));
			}
	for (const auto mode : {test::Mode::signed_unsigned,
	     test::Mode::unsigned_signed})
		for (const bool q : {false, true})
			for (unsigned index = 0; index < 4U; ++index)
				harness.require_differential(test::indexed_instruction(
					mode, q, 28U, 29U, 19U, index),
					base::state(index + q + static_cast<unsigned>(mode)));
}

} // namespace

int main()
{
	vector_sizes_and_scalar_holes();
	indexed_size_allocation();
	return 0;
}
