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

namespace arm64 = uml::backend::arm64;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::backend::tests;

using Pair = std::array<std::uint64_t, 2>;

inline constexpr std::uint64_t observed_value = 0x7264'5648'3a2c'1e10ULL;
inline constexpr std::uint64_t observed_value2 = 0xf1e2'd3c4'b5a6'9788ULL;
inline constexpr std::uint64_t operand_value = 0x6040'2000'8060'4020ULL;
inline constexpr std::uint64_t operand_value2 = 0x0f1e'2d3c'4b5a'6978ULL;
inline constexpr std::uint32_t pass_flags = 0x2000'0000U;
inline constexpr std::uint32_t compare_fail_flags = 0xa000'0000U;

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

[[nodiscard]] constexpr std::uint32_t pair_instruction(
	Operation operation, bool soft, bool acquire, bool release, unsigned rn,
	unsigned first, unsigned second)
{
	return operation == Operation::compare_swap ?
		pair_compare_swap(soft, acquire, release, rn, second, first) :
		pair_modify_raw(soft, acquire, release, code(operation), rn, first,
			second);
}

static_assert(scalar_compare_swap(false, false, false, 0, 0, 0) ==
	0x1920'0800U);
static_assert(scalar_modify_raw(false, false, false, 1, 0, 0, 0) ==
	0x3820'9000U);
static_assert(pair_compare_swap(false, false, false, 0, 0, 0) ==
	0x1920'0c00U);
static_assert(pair_modify_raw(false, false, false, 1, 0, 0, 0) ==
	0x1920'9000U);

[[nodiscard]] inline engine::NativeRequirements scalar_native()
{
	return engine::NativeRequirements::features(
		uml::backend::ArmFeature::the);
}

[[nodiscard]] inline engine::NativeRequirements pair_native()
{
	return engine::NativeRequirements::features({
		uml::backend::ArmFeature::d128,
		uml::backend::ArmFeature::the,
	});
}

struct ScalarOutcome final {
	std::uint64_t memory;
	std::uint32_t nzcv;
};

/*
 * Independent userspace oracle for MemAtomicRCW. Translation-table protection
 * is disabled in this execution environment and its software mask permits
 * every bit, leaving the descriptor state bit as the one software check. The
 * architecture permits writing the observed value back when a check fails;
 * all three engines intentionally choose that deterministic permitted result.
 */
[[nodiscard]] constexpr ScalarOutcome scalar_oracle(
	Operation operation, bool soft, std::uint64_t observed,
	std::uint64_t compare, std::uint64_t operand)
{
	std::uint64_t desired = observed;
	if (operation == Operation::compare_swap) {
		if (observed != compare)
			return {observed, compare_fail_flags};
		desired = operand;
	} else if (operation == Operation::clear) {
		desired = observed & ~operand;
	} else if (operation == Operation::swap) {
		desired = operand;
	} else {
		desired = observed | operand;
	}
	if (soft && ((observed ^ desired) & 1U) != 0)
		return {observed, 0};
	return {desired, pass_flags};
}

struct PairOutcome final {
	Pair memory;
	std::uint32_t nzcv;
};

[[nodiscard]] constexpr PairOutcome pair_oracle(
	Operation operation, bool soft, const Pair &observed,
	const Pair &compare, const Pair &operand)
{
	Pair desired = observed;
	if (operation == Operation::compare_swap) {
		if (observed != compare)
			return {observed, compare_fail_flags};
		desired = operand;
	} else if (operation == Operation::clear) {
		desired = {
			observed[0] & ~operand[0],
			observed[1] & ~operand[1],
		};
	} else if (operation == Operation::swap) {
		desired = operand;
	} else {
		desired = {
			observed[0] | operand[0],
			observed[1] | operand[1],
		};
	}
	if (soft && ((observed[0] ^ desired[0]) & 1U) != 0)
		return {observed, 0};
	return {desired, pass_flags};
}

[[nodiscard]] inline Pair read_pair(const arm64::CpuState &cpu,
	unsigned first)
{
	return {
		atomic::read_register(cpu, first),
		atomic::read_register(cpu, first + 1U),
	};
}

inline void write_pair(arm64::CpuState &cpu, unsigned first,
	const Pair &value)
{
	atomic::write_register(cpu, first, value[0]);
	atomic::write_register(cpu, first + 1U, value[1]);
}

inline void put_pair(atomic::Page &page, std::size_t offset,
	const Pair &value)
{
	atomic::put(page, offset, value[0], 8);
	atomic::put(page, offset + 8, value[1], 8);
}

[[nodiscard]] inline Pair get_pair(const atomic::Page &page,
	std::size_t offset)
{
	return {
		atomic::get(page, offset, 8),
		atomic::get(page, offset + 8, 8),
	};
}

[[nodiscard]] constexpr std::uint64_t page_base(
	std::uint64_t address)
{
	return address &
		~(static_cast<std::uint64_t>(DARWIN_UML_GUEST_PAGE_SIZE) - 1U);
}

[[nodiscard]] constexpr std::size_t page_offset(
	std::uint64_t address)
{
	return static_cast<std::size_t>(
		address & (DARWIN_UML_GUEST_PAGE_SIZE - 1U));
}

inline void check_scalar(engine::EngineHarness &harness,
	std::uint32_t instruction, Operation operation, bool soft, unsigned rn,
	unsigned source, unsigned result, std::uint64_t observed,
	const arm64::CpuState &initial, std::string_view name)
{
	const auto address = atomic::read_register(initial, rn, true);
	const auto offset = page_offset(address);
	require(offset + 8U <= DARWIN_UML_GUEST_PAGE_SIZE,
		"scalar test address crosses a page");
	const auto operand = atomic::read_register(initial, source);
	const auto compare = operation == Operation::compare_swap ?
		atomic::read_register(initial, result) : 0U;
	const auto outcome =
		scalar_oracle(operation, soft, observed, compare, operand);

	atomic::Page initial_page{};
	atomic::put(initial_page, offset, observed, 8);
	auto expected_page = initial_page;
	atomic::put(expected_page, offset, outcome.memory, 8);
	const auto memory = engine::MemoryRegion{
		.address = page_base(address),
		.initial = initial_page,
		.expected = expected_page,
	};
	auto expected = initial;
	atomic::write_register(expected, result, observed);
	expected.nzcv = outcome.nzcv;
	harness.check({
		.name = name,
		.instruction = instruction,
		.initial = initial,
		.expected = expected,
		.native = scalar_native(),
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
}

inline void check_pair(engine::EngineHarness &harness,
	std::uint32_t instruction, Operation operation, bool soft, unsigned rn,
	unsigned first, unsigned second, const Pair &observed,
	const arm64::CpuState &initial, std::string_view name)
{
	const auto address = atomic::read_register(initial, rn, true);
	const auto offset = page_offset(address);
	require(offset + 16U <= DARWIN_UML_GUEST_PAGE_SIZE,
		"pair test address crosses a page");
	Pair compare{};
	Pair operand{};
	unsigned result_first;
	unsigned result_second;
	if (operation == Operation::compare_swap) {
		operand = read_pair(initial, first);
		compare = read_pair(initial, second);
		result_first = second;
		result_second = second + 1U;
	} else {
		operand = {
			atomic::read_register(initial, first),
			atomic::read_register(initial, second),
		};
		result_first = first;
		result_second = second;
	}
	const auto outcome =
		pair_oracle(operation, soft, observed, compare, operand);

	atomic::Page initial_page{};
	put_pair(initial_page, offset, observed);
	auto expected_page = initial_page;
	put_pair(expected_page, offset, outcome.memory);
	const auto memory = engine::MemoryRegion{
		.address = page_base(address),
		.initial = initial_page,
		.expected = expected_page,
	};
	auto expected = initial;
	atomic::write_register(expected, result_first, observed[0]);
	atomic::write_register(expected, result_second, observed[1]);
	expected.nzcv = outcome.nzcv;
	harness.check({
		.name = name,
		.instruction = instruction,
		.initial = initial,
		.expected = expected,
		.native = pair_native(),
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
}

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
