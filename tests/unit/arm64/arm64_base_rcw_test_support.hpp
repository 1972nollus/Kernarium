// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_atomic_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

namespace arm64_base_rcw_test {

namespace arm64 = uml::cpu::arm64;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;

enum class Operation : unsigned {
	compare_swap = 0,
	clear = 1,
	swap = 2,
	set = 3,
};

[[noreturn]] inline void fail(std::string_view message)
{
	std::fprintf(stderr, "arm64 RCW test failure: %.*s\n",
		static_cast<int>(message.size()), message.data());
	std::abort();
}

inline void require(bool condition, std::string_view message)
{
	if (!condition)
		fail(message);
}

[[nodiscard]] constexpr unsigned code(Operation operation)
{
	return static_cast<unsigned>(operation);
}

/*
 * Test-only transcriptions of the four encoding diagrams on specification
 * pages 602, 604, 606-632. Scalar compare-and-swap uses Rs for the
 * compare/result register and Rt for the replacement. The other scalar forms
 * use Rs as the operand and Rt as the result. Pair compare-and-swap likewise
 * uses two even-register pairs; the remaining pair forms explicitly encode
 * their two arbitrary, distinct transfer registers.
 */
[[nodiscard]] constexpr std::uint32_t scalar_compare_swap(
	bool soft, bool acquire, bool release, unsigned rn, unsigned compare,
	unsigned replacement)
{
	return 0x1920'0800U | (static_cast<unsigned>(soft) << 30) |
		(static_cast<unsigned>(acquire) << 23) |
		(static_cast<unsigned>(release) << 22) |
		((compare & 31U) << 16) | ((rn & 31U) << 5) |
		(replacement & 31U);
}

[[nodiscard]] constexpr std::uint32_t scalar_modify_raw(
	bool soft, bool acquire, bool release, unsigned operation, unsigned rn,
	unsigned source, unsigned result)
{
	return 0x3820'8000U | (static_cast<unsigned>(soft) << 30) |
		(static_cast<unsigned>(acquire) << 23) |
		(static_cast<unsigned>(release) << 22) |
		((source & 31U) << 16) | ((operation & 7U) << 12) |
		((rn & 31U) << 5) | (result & 31U);
}

[[nodiscard]] constexpr std::uint32_t scalar_instruction(
	Operation operation, bool soft, bool acquire, bool release, unsigned rn,
	unsigned source, unsigned result)
{
	return operation == Operation::compare_swap ?
		scalar_compare_swap(soft, acquire, release, rn, result, source) :
		scalar_modify_raw(soft, acquire, release, code(operation), rn,
			source, result);
}

[[nodiscard]] constexpr std::uint32_t pair_compare_swap(
	bool soft, bool acquire, bool release, unsigned rn, unsigned compare,
	unsigned replacement)
{
	return 0x1920'0c00U | (static_cast<unsigned>(soft) << 30) |
		(static_cast<unsigned>(acquire) << 23) |
		(static_cast<unsigned>(release) << 22) |
		((compare & 31U) << 16) | ((rn & 31U) << 5) |
		(replacement & 31U);
}

[[nodiscard]] constexpr std::uint32_t pair_modify_raw(
	bool soft, bool acquire, bool release, unsigned operation, unsigned rn,
	unsigned first, unsigned second)
{
	return 0x1920'8000U | (static_cast<unsigned>(soft) << 30) |
		(static_cast<unsigned>(acquire) << 23) |
		(static_cast<unsigned>(release) << 22) |
		((second & 31U) << 16) | ((operation & 7U) << 12) |
		((rn & 31U) << 5) | (first & 31U);
}

static_assert(scalar_compare_swap(false, false, false, 0, 0, 0) ==
	0x1920'0800U);
static_assert(scalar_modify_raw(false, false, false, 1, 0, 0, 0) ==
	0x3820'9000U);
static_assert(pair_compare_swap(false, false, false, 0, 0, 0) ==
	0x1920'0c00U);
static_assert(pair_modify_raw(false, false, false, 1, 0, 0, 0) ==
	0x1920'9000U);

[[nodiscard]] constexpr std::uintptr_t register_offset(
	unsigned reg, bool stack_pointer = false)
{
	if (reg == 31U)
		return stack_pointer ? offsetof(arm64::CpuState, sp) : 0U;
	return offsetof(arm64::CpuState, registers) +
		reg * sizeof(std::uint64_t);
}

[[nodiscard]] constexpr std::uintptr_t scalar_stream_operand(
	unsigned rn, unsigned source, unsigned result)
{
	return register_offset(rn, true) |
		(register_offset(source) << 16) |
		(register_offset(result) << 32);
}

[[nodiscard]] constexpr std::uintptr_t pair_stream_operand(
	unsigned rn, unsigned first, unsigned second)
{
	return register_offset(rn, true) |
		(register_offset(first) << 16) |
		(register_offset(second) << 32);
}

} // namespace arm64_base_rcw_test
