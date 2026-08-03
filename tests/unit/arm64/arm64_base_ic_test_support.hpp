// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_base_feature_system_test_support.hpp"

#include <array>
#include <cstdint>

namespace arm64_base_ic_test {

struct Operation final {
	const char *name;
	unsigned op1;
	unsigned crm;
	unsigned op2;
	bool accessible_at_el0;
};

inline constexpr std::array operations{
	Operation{"IALLUIS", 0, 1, 0, false},
	Operation{"IALLU", 0, 5, 0, false},
	Operation{"IVAU", 3, 5, 1, true},
};

[[nodiscard]] constexpr bool named_operation(
	unsigned op1, unsigned crm, unsigned op2) noexcept
{
	for (const auto &operation : operations)
		if (operation.op1 == op1 && operation.crm == crm &&
		    operation.op2 == op2)
			return true;
	return false;
}

[[nodiscard]] constexpr bool named_instruction(
	std::uint32_t instruction) noexcept
{
	if ((instruction & 0xfff8'0000U) != 0xd508'0000U)
		return false;
	return named_operation((instruction >> 16) & 7U,
		(instruction >> 8) & 15U, (instruction >> 5) & 7U);
}

[[nodiscard]] constexpr std::uint32_t instruction(
	const Operation &operation, unsigned transfer) noexcept
{
	return arm64_system_test::system_instruction(false,
		operation.op1, 7, operation.crm, operation.op2, transfer);
}

} // namespace arm64_base_ic_test
