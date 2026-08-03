// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace arm64_floating_trap_test {

namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;

inline constexpr std::uint32_t fpsr_invalid = 1U << 0U;
inline constexpr std::uint32_t fpsr_divide_by_zero = 1U << 1U;
inline constexpr std::uint32_t fpsr_overflow = 1U << 2U;
inline constexpr std::uint32_t fpsr_underflow = 1U << 3U;
inline constexpr std::uint32_t fpsr_inexact = 1U << 4U;
inline constexpr std::uint32_t fpsr_input_denormal = 1U << 7U;
inline constexpr std::uint32_t fpsr_exception_mask = 0x9fU;
inline constexpr std::uint32_t fpcr_flush_to_zero = 1U << 24U;
inline constexpr std::uint32_t syndrome_base =
	(0x2cU << 26U) | (1U << 25U) | (1U << 23U);

[[nodiscard]] constexpr std::uint32_t enable_for(
	std::uint32_t flags) noexcept
{
	return (flags & fpsr_exception_mask) << 8U;
}

[[nodiscard]] constexpr std::uint32_t syndrome_for(
	std::uint32_t flags) noexcept
{
	return syndrome_base | (flags & fpsr_exception_mask);
}

[[nodiscard]] inline arm64::CpuState state(std::uint64_t seed)
{
	arm64::CpuState cpu{};
	for (std::size_t index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			0x1020'3040'5060'7080ULL ^
			(seed * 0x0101'0101ULL + index * 0x1111'1111ULL);
	for (std::size_t reg = 0; reg < cpu.vectors.size(); ++reg)
		for (std::size_t byte = 0; byte < 16; ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				(seed + reg * 19U + byte * 7U) & 0xffU);
	cpu.sp = 0x7fff'0000'0000ULL + seed * 16U;
	cpu.nzcv = static_cast<std::uint32_t>(seed << 28U) & 0xf000'0000U;
	cpu.fpsr = 1U << 27U;
	cpu.tls = 0xcafe'0000'0000'0000ULL | seed;
	cpu.fpmr = seed & 0x3f'ff7f'c1ffULL;
	return cpu;
}

inline void write_lane(arm64::CpuState &cpu, unsigned reg,
	unsigned lane, unsigned lane_bytes, std::uint64_t bits)
{
	for (unsigned byte = 0; byte < lane_bytes; ++byte)
		cpu.vectors[reg].bytes[lane * lane_bytes + byte] =
			static_cast<std::byte>(bits >> (byte * 8U));
}

inline void fill_lanes(arm64::CpuState &cpu, unsigned reg,
	unsigned lane_bytes, unsigned lanes, std::uint64_t bits)
{
	for (unsigned lane = 0; lane < lanes; ++lane)
		write_lane(cpu, reg, lane, lane_bytes, bits);
}

inline void clear_vector(arm64::CpuState &cpu, unsigned reg)
{
	cpu.vectors[reg].bytes.fill(std::byte{});
}

inline void check_trap(engine::EngineHarness &harness,
	std::string_view name, std::uint32_t instruction,
	const arm64::CpuState &initial, std::uint32_t raised_enabled,
	std::uint32_t newly_raised_unenabled,
	engine::NativeRequirements native)
{
	auto expected = initial;
	expected.fpsr |= newly_raised_unenabled & fpsr_exception_mask;
	harness.check({
		.name = name,
		.instruction = instruction,
		.initial = initial,
		.expected = expected,
		.event = engine::ExpectedEvent::floating_point_trap,
		.native = native,
		.syndrome = syndrome_for(raised_enabled),
	});
}

inline void check_illegal_with_enables(engine::EngineHarness &harness,
	std::string_view name, std::uint32_t instruction,
	const arm64::CpuState &initial,
	engine::NativeRequirements native = engine::NativeRequirements::baseline())
{
	harness.check({
		.name = name,
		.instruction = instruction,
		.initial = initial,
		.expected = initial,
		.event = engine::ExpectedEvent::illegal_instruction,
		.native = native,
		.invalid_encoding = true,
	});
}

[[nodiscard]] inline engine::NativeRequirements scalar_native() noexcept
{
	return engine::NativeRequirements::features(
		::uml::cpu::ArmFeature::floating_point);
}

[[nodiscard]] inline engine::NativeRequirements vector_native() noexcept
{
	return engine::NativeRequirements::features(
		::uml::cpu::ArmFeature::advanced_simd);
}

[[nodiscard]] inline engine::NativeRequirements scalar_half_native() noexcept
{
	return engine::NativeRequirements::features({
		::uml::cpu::ArmFeature::floating_point,
		::uml::cpu::ArmFeature::fp16,
	});
}

[[nodiscard]] inline engine::NativeRequirements vector_half_native() noexcept
{
	return engine::NativeRequirements::features({
		::uml::cpu::ArmFeature::advanced_simd,
		::uml::cpu::ArmFeature::fp16,
	});
}

static_assert(enable_for(fpsr_exception_mask) == 0x9f00U);
static_assert(syndrome_for(fpsr_invalid) == 0xb280'0001U);

} // namespace arm64_floating_trap_test
