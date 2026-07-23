// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_test_support.hpp"

namespace test = arm64_simd_test;

namespace {

[[nodiscard]] constexpr std::uint32_t widening(bool scalar, bool upper,
	bool unsigned_values, unsigned size, unsigned operation)
{
	return (scalar ? 0x5e20'0000U : 0x0e20'0000U) |
		(!scalar && upper ? 0x4000'0000U : 0U) |
		(unsigned_values ? 0x2000'0000U : 0U) |
		((size & 3U) << 22) | ((operation & 15U) << 12) |
		(2U << 16) | (1U << 5);
}

[[nodiscard]] constexpr bool valid_widening(bool scalar,
	bool unsigned_values, unsigned size, unsigned operation)
{
	if (size >= 3U)
		return false;
	const bool saturating = operation == 9U || operation == 11U ||
		operation == 13U;
	if (scalar)
		return saturating && !unsigned_values && size != 0U;
	if (saturating)
		return !unsigned_values && size != 0U;
	return operation <= 3U || operation == 5U || operation == 7U ||
		operation == 8U || operation == 10U || operation == 12U;
}

[[nodiscard]] constexpr std::uint32_t high_narrow(bool upper, bool rounding,
	bool subtract, unsigned size)
{
	return 0x0e20'4000U | (upper ? 0x4000'0000U : 0U) |
		(rounding ? 0x2000'0000U : 0U) | ((size & 3U) << 22) |
		(subtract ? 0x2000U : 0U) | (2U << 16) | (1U << 5);
}

void complete_widening_allocation()
{
	test::Harness harness;
	for (const bool scalar : {false, true}) {
		for (const bool upper : {false, true}) {
			if (scalar && upper)
				continue;
			for (const bool unsigned_values : {false, true}) {
				for (unsigned size = 0; size < 4; ++size) {
					for (unsigned operation = 0; operation < 16; ++operation) {
						/* Operations 4/6 are ADDHN/SUBHN below. Operation
						 * 14 overlaps the separately tested PMULL allocation. */
						if (operation == 4U || operation == 6U ||
						    operation == 14U)
							continue;
						const auto instruction = widening(scalar, upper,
							unsigned_values, size, operation);
						if (valid_widening(scalar, unsigned_values,
						    size, operation))
							harness.require_differential(instruction,
								test::state(operation + size));
						else
							harness.require_illegal(instruction);
					}
				}
			}
		}
	}
}

void complete_high_narrow_allocation()
{
	test::Harness harness;
	for (const bool upper : {false, true})
		for (const bool rounding : {false, true})
			for (const bool subtract : {false, true})
				for (unsigned size = 0; size < 4; ++size) {
					const auto instruction = high_narrow(upper, rounding,
						subtract, size);
					if (size < 3U)
						harness.require_differential(instruction,
							test::state(size + 7U));
					else
						harness.require_illegal(instruction);
				}
}

} // namespace

int main()
{
	complete_widening_allocation();
	complete_high_narrow_allocation();
	return 0;
}
