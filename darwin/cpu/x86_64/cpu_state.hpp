// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::x86_64 {

enum class Register : std::uint8_t {
	rax,
	rcx,
	rdx,
	rbx,
	rsp,
	rbp,
	rsi,
	rdi,
	r8,
	r9,
	r10,
	r11,
	r12,
	r13,
	r14,
	r15,
};

enum class DeferredFlagOperation : std::uint8_t {
	none,
	add,
	subtract,
	logical,
};

struct DeferredFlags final {
	std::uint64_t left{};
	std::uint64_t right{};
	std::uint64_t result{};
	DeferredFlagOperation operation{DeferredFlagOperation::none};
	std::uint8_t width{};
};

struct X87Register final {
	std::uint64_t significand{};
	std::uint16_t sign_exponent{};
};

struct CpuState final {
	std::array<std::uint64_t, 16> registers{};
	std::array<std::array<std::byte, 32>, 16> vectors{};
	std::array<std::uint64_t, 8> mmx{};
	std::array<X87Register, 8> x87{};
	std::array<std::uint16_t, 6> segment_selectors{};
	std::uint64_t rip{};
	std::uint64_t rflags{2};
	std::uint64_t fs_base{};
	std::uint64_t gs_base{};
	std::uint32_t mxcsr{0x1f80};
	std::uint16_t x87_control{0x037f};
	std::uint16_t x87_status{};
	std::uint16_t x87_tag{0xffff};
	std::uint8_t x87_top{};
	bool x87_reduced_precision{};
	bool system_call_32bit{};
	DeferredFlags deferred_flags{};

	[[nodiscard]] std::uint64_t &gpr(Register reg) noexcept
	{
		return registers[static_cast<std::uint8_t>(reg)];
	}

	[[nodiscard]] const std::uint64_t &gpr(Register reg) const noexcept
	{
		return registers[static_cast<std::uint8_t>(reg)];
	}
};

inline constexpr std::uint64_t rflags_carry = 1ULL << 0;
inline constexpr std::uint64_t rflags_reserved = 1ULL << 1;
inline constexpr std::uint64_t rflags_parity = 1ULL << 2;
inline constexpr std::uint64_t rflags_auxiliary = 1ULL << 4;
inline constexpr std::uint64_t rflags_zero = 1ULL << 6;
inline constexpr std::uint64_t rflags_sign = 1ULL << 7;
inline constexpr std::uint64_t rflags_direction = 1ULL << 10;
inline constexpr std::uint64_t rflags_overflow = 1ULL << 11;

void materialize_flags(CpuState &cpu) noexcept;

} // namespace uml::cpu::x86_64
