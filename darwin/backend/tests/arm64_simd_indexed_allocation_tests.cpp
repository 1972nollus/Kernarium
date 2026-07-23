// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_indexed_test_support.hpp"

#include <array>

namespace test = arm64_simd_indexed_test;
namespace base = arm64_simd_test;

namespace {

struct Group final {
	bool unsigned_group;
	bool scalar;
	unsigned operation;
};

void reserved_element_sizes_are_early_illegal()
{
	base::Harness harness;
	constexpr std::array groups{
		Group{true, false, 0U}, Group{true, false, 4U},
		Group{false, false, 8U},
		Group{false, false, 2U}, Group{true, false, 2U},
		Group{false, false, 6U}, Group{true, false, 6U},
		Group{false, false, 10U}, Group{true, false, 10U},
		Group{false, false, 3U}, Group{false, true, 3U},
		Group{false, false, 7U}, Group{false, true, 7U},
		Group{false, false, 11U}, Group{false, true, 11U},
		Group{false, false, 12U}, Group{false, true, 12U},
		Group{false, false, 13U}, Group{false, true, 13U},
		Group{true, false, 13U}, Group{true, true, 13U},
		Group{true, false, 15U}, Group{true, true, 15U}};
	for (const auto group : groups)
		for (const bool q : {false, true}) {
			if (group.scalar && q)
				continue;
			for (const unsigned size : {0U, 3U})
				harness.require_illegal(test::instruction(group.scalar, q,
					group.unsigned_group, size, group.operation,
					28U, 29U, 2U, 0U),
					base::state(group.operation + size + q));
		}
}

void scalar_allocation_holes_are_early_illegal()
{
	base::Harness harness;
	for (const bool unsigned_group : {false, true})
		for (unsigned operation = 0; operation < 16U; ++operation) {
			const bool allocated = unsigned_group ?
				operation == 9U || operation == 13U || operation == 15U :
				operation == 1U || operation == 3U || operation == 5U ||
				operation == 7U || operation == 9U || operation == 11U ||
				operation == 12U || operation == 13U;
			if (!allocated)
				for (const unsigned size : {1U, 2U})
					harness.require_illegal(test::instruction(true, false,
						unsigned_group, size, operation, 28U, 29U, 2U, 0U),
						base::state(operation + size + unsigned_group));
		}
	for (const bool unsigned_group : {false, true})
		for (const unsigned operation : unsigned_group ?
		     std::array{9U, 9U, 9U} : std::array{1U, 5U, 9U})
			harness.require_illegal(test::instruction(true, false,
				unsigned_group, 1U, operation, 28U, 29U, 2U, 0U));
}

} // namespace

int main()
{
	reserved_element_sizes_are_early_illegal();
	scalar_allocation_holes_are_early_illegal();
	return 0;
}
