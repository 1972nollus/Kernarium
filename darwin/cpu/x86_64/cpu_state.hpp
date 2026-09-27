// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::x86_64 {

enum class ExecutionMode : std::uint8_t {
	long64,
	compat32,
};

[[nodiscard]] constexpr std::uint64_t normalize_instruction_pointer(
	ExecutionMode mode, std::uint64_t value) noexcept
{
	return mode == ExecutionMode::compat32 ?
		static_cast<std::uint32_t>(value) : value;
}

[[nodiscard]] constexpr std::uint64_t advance_instruction_pointer(
	ExecutionMode mode, std::uint64_t value, std::uint64_t amount) noexcept
{
	return normalize_instruction_pointer(mode, value + amount);
}

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
	increment,
	decrement,
	add_carry,
	subtract_borrow,
	/* The shifts record the original value, the masked nonzero count and
	 * the result; see the engine's shift_flags_body for their flags. */
	shift_left,
	shift_right,
	shift_arithmetic_right,
};

struct DeferredFlags final {
	std::uint64_t left{};
	std::uint64_t right{};
	std::uint64_t result{};
	DeferredFlagOperation operation{DeferredFlagOperation::none};
	std::uint8_t width{};
	std::uint8_t carry{};
	/* Set on an INC or DEC record whose preserved carry has not been settled:
	 * it is whatever the record in CpuState::carry_slot defines. */
	std::uint8_t carry_pending{};
	std::uint16_t override_mask{};
	std::uint16_t override_value{};
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
	/* Compat-mode TLS bases for GDT entries 6 onward. Linux has three TLS
	 * entries (6 to 8, selectors 0x33 to 0x43), and the runtime fills and
	 * marks present only those. The array and the refresh gadget's bound
	 * still admit entries 9 and 10; a selector for either finds its present
	 * bit clear, and trimming them would move every later CpuState offset
	 * and change the bank. */
	std::array<std::uint32_t, 5> compat_tls_bases{};
	std::uint8_t compat_tls_present{};
	std::uint64_t rip{};
	std::uint64_t rflags{2};
	/* Address generation indexes {segment_zero, fs_base, gs_base} with the
	 * stream descriptor's segment selector, and names segment_zero for an
	 * absent base or index register. It must always read as zero.
	 */
	std::uint64_t segment_zero{};
	std::uint64_t fs_base{};
	std::uint64_t gs_base{};
	std::uint32_t mxcsr{0x1f80};
	std::uint16_t x87_control{0x037f};
	std::uint16_t x87_status{};
	std::uint16_t x87_tag{0xffff};
	std::uint8_t x87_top{};
	/* Unused. It keeps system_call_32bit, execution_mode and every later
	 * member at the offsets the gadget bank bakes in. */
	std::uint8_t reserved{};
	bool system_call_32bit{};
	ExecutionMode execution_mode{ExecutionMode::long64};
	/* PR_SET_TSC(PR_TSC_SIGSEGV): RDTSC and RDTSCP raise #GP. */
	bool timestamp_faults{};
	DeferredFlags deferred_flags{};
	/* The record an INC or DEC displaced without settling its carry. Only a
	 * pending INC or DEC record reads it, and only for the carry flag. */
	DeferredFlags carry_slot{};

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
inline constexpr std::uint64_t rflags_trap = 1ULL << 8;
inline constexpr std::uint64_t rflags_direction = 1ULL << 10;
inline constexpr std::uint64_t rflags_overflow = 1ULL << 11;

void materialize_flags(CpuState &cpu) noexcept;

} // namespace uml::cpu::x86_64
