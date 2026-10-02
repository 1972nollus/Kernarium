// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_system_test_support.hpp"

#include <cstdint>
#include <string_view>

namespace uml::emulator::tests::base_pstate {

namespace arm64 = uml::cpu::arm64;

inline constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

enum class Field : std::uint8_t {
	flag_alias,
	reserved,
	uao,
	pan,
	stack_pointer_select,
	all_interrupt_mask,
	profiling_mask,
	ssbs,
	dit,
	streaming_mode,
	array_state,
	streaming_and_array_state,
	tag_check_override,
	interrupt_mask_set,
	interrupt_mask_clear,
};

struct Decoded final {
	Field field{Field::reserved};
	bool allocated{};
	bool el0_accessible{};
};

[[nodiscard]] constexpr std::uint32_t encode(
	unsigned op1, unsigned immediate, unsigned op2) noexcept
{
	return 0xd500'401fU | ((op1 & 7U) << 16U) |
		((immediate & 15U) << 8U) | ((op2 & 7U) << 5U);
}

[[nodiscard]] constexpr bool is_flag_alias(
	unsigned op1, unsigned immediate, unsigned op2) noexcept
{
	return op1 == 0U && immediate == 0U && op2 <= 2U;
}

/*
 * Independent transcription of the MSR (immediate) allocation tables.  The
 * emulator runs an EL0 guest process, so architecturally allocated fields with
 * a higher minimum exception level are distinguished from reserved encodings.
 */
[[nodiscard]] inline Decoded decode(
	unsigned op1, unsigned immediate, unsigned op2) noexcept
{
	if (is_flag_alias(op1, immediate, op2))
		return {
			.field = Field::flag_alias,
			.allocated = true,
			.el0_accessible = true,
		};

	Field field = Field::reserved;
	bool allocated = false;
	bool el0_accessible = false;
	if (op1 == 0U) {
		switch (op2) {
		case 3U:
			field = Field::uao;
			allocated = true;
			break;
		case 4U:
			field = Field::pan;
			allocated = true;
			break;
		case 5U:
			field = Field::stack_pointer_select;
			allocated = true;
			break;
		default:
			break;
		}
	} else if (op1 == 1U && op2 == 0U) {
		if (immediate <= 1U) {
			field = Field::all_interrupt_mask;
			allocated = true;
		} else if (immediate <= 3U) {
			field = Field::profiling_mask;
			allocated = true;
		}
	} else if (op1 == 3U) {
		el0_accessible = true;
		switch (op2) {
		case 1U:
			field = Field::ssbs;
			allocated = true;
			break;
		case 2U:
			field = Field::dit;
			allocated = true;
			break;
		case 3U:
			if (immediate < 8U) {
				switch ((immediate >> 1U) & 3U) {
				case 1U:
					field = Field::streaming_mode;
					allocated = true;
					break;
				case 2U:
					field = Field::array_state;
					allocated = true;
					break;
				case 3U:
					field = Field::streaming_and_array_state;
					allocated = true;
					break;
				default:
					break;
				}
			}
			break;
		case 4U:
			field = Field::tag_check_override;
			allocated = true;
			break;
		case 6U:
			field = Field::interrupt_mask_set;
			allocated = true;
			break;
		case 7U:
			field = Field::interrupt_mask_clear;
			allocated = true;
			break;
		default:
			break;
		}
	}
	if (!allocated)
		el0_accessible = false;
	return {
		.field = field,
		.allocated = allocated,
		.el0_accessible = el0_accessible,
	};
}

} // namespace uml::emulator::tests::base_pstate
