// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>

namespace uml::backend::tests::base_control {

namespace arm64 = uml::backend::arm64;

inline constexpr std::uint32_t terminal_system_call = 0xd400'0001U;
inline constexpr std::size_t branch_entry_offset =
	DARWIN_UML_GUEST_PAGE_SIZE / 2;
inline constexpr std::uint64_t branch_pc =
	engine_test_code_base + branch_entry_offset;

[[noreturn]] inline void fail(const char *message)
{
	std::fprintf(stderr, "%s\n", message);
	std::abort();
}

inline void require(bool condition, const char *message)
{
	if (!condition)
		fail(message);
}

[[nodiscard]] inline arm64::CpuState seeded_state(std::uint64_t seed)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < cpu.registers.size(); ++index) {
		const auto ordinal = static_cast<std::uint64_t>(index + 1U);
		cpu.registers[index] =
			(seed ^ (ordinal * 0x9e37'79b9'7f4a'7c15ULL)) +
			ordinal * 0x0102'0304'0506'0708ULL;
	}
	cpu.sp = seed ^ 0xfedc'ba98'7654'3210ULL;
	cpu.nzcv = static_cast<std::uint32_t>((seed >> 7) & 15U) << 28;
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size();
		     ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed + reg * 29U + byte * 17U);
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'009fU;
	cpu.tls = seed ^ 0x0123'4567'89ab'cdefULL;
	cpu.set_mte_configuration(true, 0x5a5aU);
	cpu.set_mte_generator(true, 0x1357U, 9U,
		seed ^ 0x6a09'e667'f3bc'c909ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = static_cast<std::uint32_t>(seed);
	cpu.pstate_ssbs = true;
	cpu.pstate_dit = true;
	cpu.pstate_tco = true;
	cpu.pstate_sm = true;
	cpu.pstate_za = true;
	cpu.pstate_daif_access = true;
	cpu.pstate_daif = 0xbU;
	cpu.exclusive_address = seed ^ 0x8877'6655'4433'2210ULL;
	cpu.exclusive_key = static_cast<std::uintptr_t>(
		seed ^ 0x1020'3040'5060'7080ULL);
	cpu.exclusive_value = seed ^ 0x1111'2222'3333'4444ULL;
	cpu.exclusive_value2 = seed ^ 0x5555'6666'7777'8888ULL;
	cpu.exclusive_version = seed ^ 0x9999'aaaa'bbbb'ccccULL;
	cpu.exclusive_size = 8;
	cpu.fpmr = seed ^ 0x0f1e'2d3c'4b5a'6978ULL;
	return cpu;
}

[[nodiscard]] constexpr std::int64_t sign_extend(
	std::uint64_t value, unsigned bits) noexcept
{
	const auto shift = 64U - bits;
	return static_cast<std::int64_t>(value << shift) >> shift;
}

[[nodiscard]] constexpr bool condition_holds(
	std::uint32_t nzcv, unsigned condition) noexcept
{
	const bool negative = (nzcv & 0x8000'0000U) != 0;
	const bool zero = (nzcv & 0x4000'0000U) != 0;
	const bool carry = (nzcv & 0x2000'0000U) != 0;
	const bool overflow = (nzcv & 0x1000'0000U) != 0;
	bool result = false;
	switch ((condition >> 1) & 7U) {
	case 0: result = zero; break;
	case 1: result = carry; break;
	case 2: result = negative; break;
	case 3: result = overflow; break;
	case 4: result = carry && !zero; break;
	case 5: result = negative == overflow; break;
	case 6: result = !zero && negative == overflow; break;
	case 7: result = true; break;
	}
	return condition < 14U && (condition & 1U) != 0 ? !result : result;
}

[[nodiscard]] constexpr std::uint32_t encode_pc_relative(
	bool page, std::int64_t immediate, unsigned destination) noexcept
{
	const auto encoded =
		static_cast<std::uint64_t>(immediate) & 0x1f'ffffU;
	return (page ? 0x9000'0000U : 0x1000'0000U) |
		((encoded & 3U) << 29) | ((encoded >> 2) << 5) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_direct_branch(
	bool link, std::int64_t displacement) noexcept
{
	const auto immediate =
		(static_cast<std::uint64_t>(displacement) >> 2) & 0x03ff'ffffU;
	return (link ? 0x9400'0000U : 0x1400'0000U) | immediate;
}

[[nodiscard]] constexpr std::uint32_t encode_conditional_branch(
	bool consistent, unsigned condition, std::int64_t displacement) noexcept
{
	const auto immediate =
		(static_cast<std::uint64_t>(displacement) >> 2) & 0x7ffffU;
	return 0x5400'0000U | (immediate << 5) |
		(consistent ? 0x10U : 0U) | (condition & 15U);
}

[[nodiscard]] constexpr std::uint32_t encode_compare_branch(
	bool wide, bool nonzero, std::int64_t displacement,
	unsigned source) noexcept
{
	const auto immediate =
		(static_cast<std::uint64_t>(displacement) >> 2) & 0x7ffffU;
	return 0x3400'0000U | (wide ? 0x8000'0000U : 0U) |
		(nonzero ? 0x0100'0000U : 0U) | (immediate << 5) |
		(source & 31U);
}

[[nodiscard]] constexpr std::uint32_t encode_test_branch(
	bool nonzero, unsigned bit, std::int64_t displacement,
	unsigned source) noexcept
{
	const auto immediate =
		(static_cast<std::uint64_t>(displacement) >> 2) & 0x3fffU;
	return 0x3600'0000U | ((bit >> 5) << 31) |
		(nonzero ? 0x0100'0000U : 0U) | ((bit & 31U) << 19) |
		(immediate << 5) | (source & 31U);
}

enum class RegisterBranch {
	branch,
	branch_link,
	return_,
};

[[nodiscard]] constexpr std::uint32_t encode_register_branch(
	RegisterBranch operation, unsigned source) noexcept
{
	const auto base = operation == RegisterBranch::branch ? 0xd61f'0000U :
		operation == RegisterBranch::branch_link ? 0xd63f'0000U :
		0xd65f'0000U;
	return base | ((source & 31U) << 5);
}

[[nodiscard]] constexpr bool in_code_page(std::uint64_t address) noexcept
{
	return address >= engine_test_code_base &&
		address <= engine_test_code_base + DARWIN_UML_GUEST_PAGE_SIZE -
			sizeof(std::uint32_t);
}

[[nodiscard]] constexpr std::uint64_t page_address(
	std::uint64_t address) noexcept
{
	return address &
		~(std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE} - 1U);
}

class Harness final {
public:
	void check_linear(std::string_view name, std::uint32_t instruction,
		const arm64::CpuState &initial,
		const arm64::CpuState &expected,
		NativeRequirements native = NativeRequirements::baseline())
	{
		engines_.check_one(name, instruction, initial, expected, native);
	}

	void check_branch(std::string_view name, std::uint32_t instruction,
		const arm64::CpuState &initial,
		const arm64::CpuState &post_branch, std::uint64_t next_pc,
		NativeRequirements native = NativeRequirements::baseline())
	{
		const bool loops = next_pc == branch_pc;
		std::array<ProgramWord, 2> code{
			ProgramWord{
				.offset = branch_entry_offset,
				.instruction = instruction,
			},
			ProgramWord{},
		};
		std::size_t code_size = 1;
		std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> target_page{};
		MemoryRegion target_region{};
		std::span<const MemoryRegion> memory;

		if (!loops) {
			if (in_code_page(next_pc)) {
				code[code_size++] = {
					.offset = static_cast<std::size_t>(
						next_pc - engine_test_code_base),
					.instruction = terminal_system_call,
				};
			} else {
				const auto target_base = page_address(next_pc);
				const auto target_offset = static_cast<std::size_t>(
					next_pc - target_base);
				std::memcpy(target_page.data() + target_offset,
					&terminal_system_call,
					sizeof(terminal_system_call));
				target_region = {
					.address = target_base,
					.initial =
						std::span<const std::byte>{target_page},
					.readable = true,
					.writable = true,
					.executable = true,
					.guest_code = true,
					.guest_writable = true,
				};
				memory = {&target_region, 1};
			}
		}

		auto expected = post_branch;
		expected.pc = loops ? branch_pc :
			next_pc + sizeof(std::uint32_t);
		engines_.check_program({
			.name = name,
			.code = {code.data(), code_size},
			.entry_offset = branch_entry_offset,
			.initial = initial,
			.expected = expected,
			.event = loops ? ExpectedEvent::instruction_budget :
				ExpectedEvent::system_call,
			.native = native,
			.memory = memory,
		});
	}

	void check_register_branch(
		std::string_view name, std::uint32_t instruction,
		const arm64::CpuState &initial,
		const arm64::CpuState &post_branch, std::uint64_t target)
	{
		const std::array code{
			ProgramWord{
				.offset = branch_entry_offset,
				.instruction = instruction,
			},
		};
		auto expected = post_branch;
		expected.pc = target;
		engines_.check_program({
			.name = name,
			.code = std::span<const ProgramWord>{code},
			.entry_offset = branch_entry_offset,
			.initial = initial,
			.expected = expected,
			.event = ExpectedEvent::memory_fault,
			.native = NativeRequirements::baseline(),
		});
	}

	void print_summary(std::string_view suite) const
	{
		engines_.print_summary(suite);
	}

private:
	EngineHarness engines_;
};

} // namespace uml::backend::tests::base_control
