// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"
#include "uml_darwin_guest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <source_location>
#include <span>
#include <string_view>
#include <vector>

namespace mops_test {

namespace arm64 = uml::cpu::arm64;
namespace gadgets = uml::cpu::arm64::gadgets;
namespace engine = uml::emulator::tests;

inline constexpr std::uint64_t data_base = engine::engine_test_data_base;
inline constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr std::uint64_t address_mask = 0x00ff'ffff'ffff'ffffULL;
inline constexpr std::uint64_t max_block = 0x7fff'ffff'ffff'ffffULL;
inline constexpr std::uint64_t max_copy = 0x007f'ffff'ffff'ffffULL;
inline constexpr std::uint64_t max_tag_set = 0x7fff'ffff'ffff'fff0ULL;
inline constexpr std::uint32_t option_b_forward = 0x2000'0000U;
inline constexpr std::uint32_t option_b_backward = 0xa000'0000U;
inline constexpr std::uint32_t system_call = 0xd400'0001U;

enum class Stage : unsigned {
	prologue,
	main,
	epilogue,
};

[[nodiscard]] constexpr std::uint32_t raw_instruction(unsigned size,
	unsigned family, unsigned stage, unsigned options, unsigned destination,
	unsigned source, unsigned count) noexcept
{
	return 0x1900'0400U | ((size & 3U) << 30) |
		((family & 1U) << 26) | ((stage & 3U) << 22) |
		((source & 31U) << 16) | ((options & 15U) << 12) |
		((count & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t copy_instruction(bool general,
	Stage stage, unsigned options, unsigned destination, unsigned source,
	unsigned count) noexcept
{
	return raw_instruction(0, general, static_cast<unsigned>(stage), options,
		destination, source, count);
}

[[nodiscard]] constexpr std::uint32_t set_instruction(Stage stage,
	unsigned options, unsigned destination, unsigned count,
	unsigned source) noexcept
{
	return 0x19c0'0400U | (static_cast<unsigned>(stage) << 14) |
		((source & 31U) << 16) | ((options & 3U) << 12) |
		((count & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t set_tag_instruction(Stage stage,
	unsigned options, unsigned destination, unsigned count,
	unsigned source) noexcept
{
	return 0x1dc0'0400U | (static_cast<unsigned>(stage) << 14) |
		((source & 31U) << 16) | ((options & 3U) << 12) |
		((count & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr bool valid_copy_registers(unsigned destination,
	unsigned source, unsigned count) noexcept
{
	return destination != 31U && source != 31U && count != 31U &&
		destination != source && destination != count && source != count;
}

[[nodiscard]] constexpr bool valid_set_registers(unsigned destination,
	unsigned count, unsigned source) noexcept
{
	return destination != 31U && count != 31U && destination != count &&
		source != destination && source != count;
}

[[nodiscard]] constexpr bool allocated_raw(unsigned size, unsigned stage,
	unsigned options) noexcept
{
	return size == 0U &&
		(stage != 3U || options < 12U);
}

[[nodiscard]] inline engine::NativeRequirements native(
	bool tags = false) noexcept
{
	return tags ?
		engine::NativeRequirements::features({
			::uml::cpu::ArmFeature::mops,
			::uml::cpu::ArmFeature::mte,
		}) :
		engine::NativeRequirements::features(
			::uml::cpu::ArmFeature::mops);
}

[[noreturn]] inline void fail(std::string_view message,
	const std::source_location location = std::source_location::current())
{
	std::fprintf(stderr, "MOPS test failure: %.*s at %s:%u\n",
		static_cast<int>(message.size()), message.data(),
		location.file_name(), location.line());
	std::abort();
}

inline void require(bool condition, std::string_view message,
	const std::source_location location = std::source_location::current())
{
	if (!condition)
		fail(message, location);
}

inline void check_runtime_illegal(engine::EngineHarness &harness,
	std::string_view name, std::uint32_t instruction,
	const arm64::CpuState &initial, engine::NativeRequirements requirements)
{
	/*
	 * The encoding is allocated, so an incapable host still skips JIT. On a
	 * capable host all engines diagnose the malformed post-prologue state.
	 */
	harness.check({
		.name = name,
		.instruction = instruction,
		.initial = initial,
		.expected = initial,
		.event = engine::ExpectedEvent::illegal_instruction,
		.native = requirements,
	});
}

[[nodiscard]] inline arm64::CpuState state()
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] =
			0x1020'3040'5060'7080ULL + index * 0x0101'0101'0101'0101ULL;
	cpu.sp = 0xfedc'ba98'7654'3210ULL;
	cpu.nzcv = 0xd000'0000U;
	for (unsigned vector = 0; vector < cpu.vectors.size(); ++vector)
		for (unsigned byte = 0; byte < cpu.vectors[vector].bytes.size(); ++byte)
			cpu.vectors[vector].bytes[byte] = static_cast<std::byte>(
				vector * 17U + byte * 11U + 3U);
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'009fU;
	cpu.fpmr = 0x0000'0000'0000'4321ULL;
	cpu.tls = 0x1234'5678'9abc'def0ULL;
	cpu.set_mte_configuration(true, 0x8421U);
	cpu.set_mte_generator(true, 0xace1U, 7, 0xfeed'face'0123'4567ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = 0xa1b2'c3d4U;
	cpu.pstate_ssbs = true;
	cpu.pstate_dit = true;
	cpu.pstate_tco = true;
	cpu.pstate_sm = true;
	cpu.pstate_za = true;
	cpu.pstate_daif_access = true;
	cpu.pstate_daif = 0xbU;
	cpu.exclusive_address = 0x7654'3000ULL;
	cpu.exclusive_key = 0x1234U;
	cpu.exclusive_value = 0xa5a5'a5a5'a5a5'a5a5ULL;
	cpu.exclusive_value2 = 0x5a5a'5a5a'5a5a'5a5aULL;
	cpu.exclusive_version = 19;
	cpu.exclusive_size = 16;
	cpu.gcs_pointer = 0x7788'9900ULL;
	cpu.gcs_implemented = true;
	cpu.gcs_push_enabled = true;
	cpu.gcs_store_enabled = true;
	return cpu;
}

inline void fill_pattern(std::span<std::byte> bytes, unsigned salt = 0)
{
	for (std::size_t index = 0; index < bytes.size(); ++index)
		bytes[index] = static_cast<std::byte>(
			(index * 53U + salt * 29U + 0x37U) & 0xffU);
}

inline void copy_bytes(std::vector<std::byte> &bytes, std::size_t destination,
	std::size_t source, std::size_t size)
{
	require(destination <= bytes.size() &&
			size <= bytes.size() - destination &&
			source <= bytes.size() && size <= bytes.size() - source,
		"copy oracle range");
	std::memmove(bytes.data() + destination, bytes.data() + source, size);
}

inline void set_bytes(std::vector<std::byte> &bytes, std::size_t destination,
	std::size_t size, std::byte value)
{
	require(destination <= bytes.size() &&
			size <= bytes.size() - destination,
		"set oracle range");
	std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(destination),
		size, value);
}

[[nodiscard]] constexpr std::uint64_t tagged_address(
	std::uint64_t address, unsigned tag) noexcept
{
	return (address & address_mask) |
		(static_cast<std::uint64_t>(tag & 15U) << 56);
}

[[nodiscard]] constexpr std::uint64_t successful_program_pc(
	std::size_t instruction_count) noexcept
{
	return engine::engine_test_code_base +
		instruction_count * sizeof(std::uint32_t);
}

} // namespace mops_test
