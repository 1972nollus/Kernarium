// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_system_test_support.hpp"

#include <cstdint>
#include <string_view>

namespace uml::backend::tests::base_pstate {

namespace arm64 = uml::backend::arm64;

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
	NativeRequirements native{NativeRequirements::baseline()};
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

[[nodiscard]] inline NativeRequirements requirements_for(Field field) noexcept
{
	switch (field) {
	case Field::uao:
		return NativeRequirements::features(ArmFeature::uao);
	case Field::pan:
		return NativeRequirements::features(ArmFeature::pan);
	case Field::all_interrupt_mask:
		return NativeRequirements::features(ArmFeature::nmi);
	case Field::profiling_mask:
		return NativeRequirements::features(ArmFeature::ebep);
	case Field::ssbs:
		return NativeRequirements::features(ArmFeature::ssbs);
	case Field::dit:
		return NativeRequirements::features(ArmFeature::dit);
	case Field::streaming_mode:
	case Field::array_state:
	case Field::streaming_and_array_state:
		return NativeRequirements::features(ArmFeature::sme);
	case Field::tag_check_override:
		return NativeRequirements::features(ArmFeature::mte);
	case Field::flag_alias:
	case Field::reserved:
	case Field::stack_pointer_select:
	case Field::interrupt_mask_set:
	case Field::interrupt_mask_clear:
		return NativeRequirements::baseline();
	}
	return NativeRequirements::baseline();
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
			.native = op2 == 0U ?
				NativeRequirements::features(ArmFeature::flag_m) :
				NativeRequirements::features(ArmFeature::flag_m2),
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
		.native = requirements_for(field),
	};
}

[[nodiscard]] inline arm64::CpuState seeded_state(
	std::uint64_t seed, bool interrupt_mask_access = false)
{
	auto cpu = arm64_system_test::state();
	cpu.registers[0] ^= seed;
	cpu.registers[17] += seed * 0x9e37'79b9U;
	cpu.sp ^= seed << 4U;
	cpu.pstate_ssbs = (seed & 1U) != 0;
	cpu.pstate_dit = (seed & 2U) != 0;
	cpu.pstate_tco = (seed & 4U) != 0;
	cpu.pstate_sm = (seed & 8U) != 0;
	cpu.pstate_za = (seed & 16U) != 0;
	cpu.pstate_daif_access = interrupt_mask_access;
	cpu.pstate_daif = static_cast<std::uint8_t>((seed >> 5U) & 15U);
	return cpu;
}

[[nodiscard]] inline bool apply(
	arm64::CpuState &cpu, Field field, unsigned immediate) noexcept
{
	const bool value = (immediate & 1U) != 0;
	switch (field) {
	case Field::ssbs:
		cpu.pstate_ssbs = value;
		return true;
	case Field::dit:
		cpu.pstate_dit = value;
		return true;
	case Field::streaming_mode:
		cpu.pstate_sm = value;
		return true;
	case Field::array_state:
		cpu.pstate_za = value;
		return true;
	case Field::streaming_and_array_state:
		cpu.pstate_sm = value;
		cpu.pstate_za = value;
		return true;
	case Field::tag_check_override:
		cpu.pstate_tco = value;
		return true;
	case Field::interrupt_mask_set:
		if (!cpu.pstate_daif_access)
			return false;
		cpu.pstate_daif = static_cast<std::uint8_t>(
			(cpu.pstate_daif | immediate) & 15U);
		return true;
	case Field::interrupt_mask_clear:
		if (!cpu.pstate_daif_access)
			return false;
		cpu.pstate_daif = static_cast<std::uint8_t>(
			cpu.pstate_daif & ~immediate & 15U);
		return true;
	case Field::flag_alias:
	case Field::reserved:
	case Field::uao:
	case Field::pan:
	case Field::stack_pointer_select:
	case Field::all_interrupt_mask:
	case Field::profiling_mask:
		return false;
	}
	return false;
}

inline void check_trap(EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, const arm64::CpuState &initial,
	NativeRequirements native)
{
	harness.check({
		.name = name,
		.instruction = instruction,
		.initial = initial,
		.expected = initial,
		.event = ExpectedEvent::illegal_instruction,
		.native = native,
		.invalid_encoding = false,
	});
}

inline void check_reserved(EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	harness.check_illegal(name, instruction, initial);
}

inline void check_allocated(EngineHarness &harness, std::string_view name,
	std::uint32_t instruction, const arm64::CpuState &initial,
	const Decoded &decoded, unsigned immediate)
{
	arm64_system_test::require(decoded.allocated &&
		decoded.el0_accessible && decoded.field != Field::flag_alias);
	auto expected = initial;
	if (!apply(expected, decoded.field, immediate)) {
		check_trap(harness, name, instruction, initial, decoded.native);
		return;
	}
	harness.check_one(name, instruction, initial, expected, decoded.native);
}

} // namespace uml::backend::tests::base_pstate
