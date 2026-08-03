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

namespace arm64_base_lse128_pair_test {

namespace arm64 = uml::cpu::arm64;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;

using Pair = std::array<std::uint64_t, 2>;

enum class Operation : unsigned {
	clear = 1,
	set = 3,
	swap = 8,
};

inline constexpr Pair observed{
	0xf0f1'f2f3'f4f5'f6f7ULL,
	0x0f1e'2d3c'4b5a'6978ULL,
};
inline constexpr Pair operand{
	0x3300'5500'7700'9900ULL,
	0xcc00'aa00'8800'6600ULL,
};

[[noreturn]] inline void fail(std::string_view message)
{
	std::fprintf(stderr, "LSE128 pair test failure: %.*s\n",
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
 * Independent transcription of the "128-bit atomic memory operations"
 * encoding table. S is exposed only for allocation-boundary tests; all three
 * instructions implemented here require S=0.
 */
[[nodiscard]] constexpr std::uint32_t raw(
	bool software, bool acquire, bool release, bool o3, unsigned opcode,
	unsigned rn, unsigned rt, unsigned rt2)
{
	return 0x1920'0000U |
		(static_cast<unsigned>(software) << 30) |
		(static_cast<unsigned>(acquire) << 23) |
		(static_cast<unsigned>(release) << 22) |
		((rt2 & 31U) << 16) |
		(static_cast<unsigned>(o3) << 15) |
		((opcode & 7U) << 12) |
		((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t instruction(
	Operation operation, bool acquire, bool release, unsigned rn,
	unsigned rt, unsigned rt2)
{
	const bool swap = operation == Operation::swap;
	return raw(false, acquire, release, swap,
		swap ? 0U : code(operation), rn, rt, rt2);
}

static_assert(instruction(Operation::clear, false, false, 0, 0, 1) ==
	0x1921'1000U);
static_assert(instruction(Operation::set, true, false, 0, 0, 1) ==
	0x19a1'3000U);
static_assert(instruction(Operation::swap, true, true, 0, 0, 1) ==
	0x19e1'8000U);

[[nodiscard]] constexpr Pair oracle(
	Operation operation, const Pair &old_value, const Pair &source)
{
	if (operation == Operation::clear)
		return {
			old_value[0] & ~source[0],
			old_value[1] & ~source[1],
		};
	if (operation == Operation::set)
		return {
			old_value[0] | source[0],
			old_value[1] | source[1],
		};
	return source;
}

inline void put_pair(atomic::Page &page, std::size_t offset,
	const Pair &value)
{
	atomic::put(page, offset, value[0], 8);
	atomic::put(page, offset + 8, value[1], 8);
}

[[nodiscard]] inline Pair read_pair(
	const arm64::CpuState &cpu, unsigned first, unsigned second)
{
	return {
		atomic::read_register(cpu, first),
		atomic::read_register(cpu, second),
	};
}

inline void write_pair(
	arm64::CpuState &cpu, unsigned first, unsigned second, const Pair &value)
{
	atomic::write_register(cpu, first, value[0]);
	atomic::write_register(cpu, second, value[1]);
}

struct ExpectedCounts final {
	std::size_t cases{};
	std::size_t jit{};
	std::size_t feature_skips{};

	void add(engine::NativeRequirements requirements, bool invalid = false)
	{
		++cases;
		if (invalid || engine::host_supports(requirements))
			++jit;
		else
			++feature_skips;
	}
};

inline void require_counts(
	const engine::EngineHarness &harness, const ExpectedCounts &expected)
{
	const auto &actual = harness.counts();
	require(actual.interpreter == expected.cases,
		"unexpected interpreter case count");
	require(actual.gadgets == expected.cases,
		"unexpected gadget case count");
	require(actual.jit == expected.jit, "unexpected JIT case count");
	require(actual.jit_skipped == expected.feature_skips,
		"unexpected JIT skip count");
	require(actual.jit_skipped_host_feature == expected.feature_skips,
		"unexpected JIT host-feature skip count");
	require(actual.jit_unavailable == 0U, "native JIT engine unavailable");
}

inline void check(
	engine::EngineHarness &harness, ExpectedCounts &counts,
	std::string_view name, Operation operation, bool acquire, bool release,
	unsigned rn, unsigned rt, unsigned rt2, const arm64::CpuState &initial,
	const Pair &old_value = observed)
{
	atomic::Page initial_page{};
	for (std::size_t index = 0; index < initial_page.size(); ++index)
		initial_page[index] =
			static_cast<std::byte>((index * 37U + code(operation) * 19U) &
				0xffU);
	put_pair(initial_page, atomic::data_offset, old_value);
	auto expected_page = initial_page;
	const auto source = read_pair(initial, rt, rt2);
	put_pair(expected_page, atomic::data_offset,
		oracle(operation, old_value, source));
	auto expected = initial;
	write_pair(expected, rt, rt2, old_value);
	const auto memory = atomic::region(initial_page, expected_page);
	const auto native = atomic::lse128();
	harness.check({
		.name = name,
		.instruction = instruction(operation, acquire, release, rn, rt, rt2),
		.initial = initial,
		.expected = expected,
		.native = native,
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
	counts.add(native);
}

inline void check_illegal(
	engine::EngineHarness &harness, ExpectedCounts &counts,
	std::string_view name, std::uint32_t value,
	const arm64::CpuState &initial)
{
	const auto native = atomic::lse128();
	harness.check_illegal(name, value, initial, native);
	counts.add(native, true);
}

} // namespace arm64_base_lse128_pair_test
