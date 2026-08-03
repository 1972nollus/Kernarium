// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::cpu::arm64::detail {

enum class GcsOperation : std::uint8_t {
	none,
	pop_exception_context,
	pop_main,
	pop_exception,
	push_main,
	push_exception,
	switch_stack_1,
	switch_stack_2,
	store,
	store_unprivileged,
};

struct GcsInstruction final {
	GcsOperation operation{GcsOperation::none};
	unsigned transfer{};
	unsigned base{};

	[[nodiscard]] constexpr explicit operator bool() const noexcept
	{
		return operation != GcsOperation::none;
	}
};

[[nodiscard]] constexpr GcsInstruction
decode_gcs(std::uint32_t instruction) noexcept
{
	switch (instruction & 0xffff'ffe0U) {
	case 0xd508'77a0U:
		return {GcsOperation::pop_exception_context, instruction & 31U, 0};
	case 0xd52b'7720U:
		return {GcsOperation::pop_main, instruction & 31U, 0};
	case 0xd508'77c0U:
		return {GcsOperation::pop_exception, instruction & 31U, 0};
	case 0xd50b'7700U:
		return {GcsOperation::push_main, instruction & 31U, 0};
	case 0xd508'7780U:
		return {GcsOperation::push_exception, instruction & 31U, 0};
	case 0xd50b'7740U:
		return {GcsOperation::switch_stack_1, instruction & 31U, 0};
	case 0xd52b'7760U:
		return {GcsOperation::switch_stack_2, instruction & 31U, 0};
	default:
		break;
	}

	switch (instruction & 0xffff'fc00U) {
	case 0xd91f'0c00U:
		return {GcsOperation::store, instruction & 31U,
			(instruction >> 5) & 31U};
	case 0xd91f'1c00U:
		return {GcsOperation::store_unprivileged, instruction & 31U,
			(instruction >> 5) & 31U};
	default:
		return {};
	}
}

[[nodiscard]] constexpr bool
gcs_is_privileged_exception_operation(GcsOperation operation) noexcept
{
	return operation == GcsOperation::pop_exception_context ||
		operation == GcsOperation::pop_exception ||
		operation == GcsOperation::push_exception;
}

[[nodiscard]] constexpr bool
gcs_writes_memory(GcsOperation operation) noexcept
{
	return operation == GcsOperation::push_main ||
		operation == GcsOperation::switch_stack_1 ||
		operation == GcsOperation::switch_stack_2 ||
		operation == GcsOperation::store ||
		operation == GcsOperation::store_unprivileged;
}

} // namespace uml::cpu::arm64::detail
