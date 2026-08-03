// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_atomic_engine_test_support.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

namespace arm64_base_lsui_pair_test {

namespace arm64 = uml::cpu::arm64;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;

inline constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr std::uint64_t data_page = engine::engine_test_data_base;
inline constexpr std::uint64_t base_address =
	data_page + page_size / 2U;

enum class Mode : unsigned {
	non_temporal = 0,
	post_index = 1,
	signed_offset = 2,
	pre_index = 3,
};

struct Variant final {
	bool load;
	Mode mode;
	std::string_view mnemonic;
};

inline constexpr std::array variants{
	Variant{false, Mode::non_temporal, "STTNP"},
	Variant{true, Mode::non_temporal, "LDTNP"},
	Variant{false, Mode::post_index, "STTP-post"},
	Variant{true, Mode::post_index, "LDTP-post"},
	Variant{false, Mode::signed_offset, "STTP-offset"},
	Variant{true, Mode::signed_offset, "LDTP-offset"},
	Variant{false, Mode::pre_index, "STTP-pre"},
	Variant{true, Mode::pre_index, "LDTP-pre"},
};

[[noreturn]] inline void fail(std::string_view message)
{
	std::fprintf(stderr, "LSUI pair test failure: %.*s\n",
		static_cast<int>(message.size()), message.data());
	std::abort();
}

inline void require(bool condition, std::string_view message)
{
	if (!condition)
		fail(message);
}

[[nodiscard]] constexpr unsigned code(Mode mode)
{
	return static_cast<unsigned>(mode);
}

/*
 * Test-only transcription of the FEAT_LSUI pair diagrams. opc=11 fixes the
 * 64-bit element width; mode 0 is the non-temporal form, while modes 1/2/3
 * select post-index, signed-offset, and pre-index respectively.
 */
[[nodiscard]] constexpr std::uint32_t instruction(
	bool load, Mode mode, unsigned imm7, unsigned rn, unsigned rt,
	unsigned rt2)
{
	return 0x2800'0000U | (3U << 30) |
		(static_cast<unsigned>(load) << 22) |
		((code(mode) & 3U) << 23) |
		((imm7 & 0x7fU) << 15) |
		((rt2 & 31U) << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

static_assert(instruction(false, Mode::non_temporal, 0, 0, 0, 0) ==
	0xe800'0000U);
static_assert(instruction(true, Mode::non_temporal, 0, 0, 0, 0) ==
	0xe840'0000U);
static_assert(instruction(false, Mode::post_index, 0, 0, 0, 0) ==
	0xe880'0000U);
static_assert(instruction(true, Mode::signed_offset, 0, 0, 0, 0) ==
	0xe940'0000U);
static_assert(instruction(false, Mode::pre_index, 0, 0, 0, 0) ==
	0xe980'0000U);
static_assert(instruction(true, Mode::pre_index, 0, 0, 0, 0) ==
	0xe9c0'0000U);

[[nodiscard]] constexpr std::int64_t signed_imm7(unsigned imm7)
{
	return static_cast<std::int64_t>(
		static_cast<std::uint64_t>(imm7 & 0x7fU) << 57) >> 57;
}

[[nodiscard]] constexpr std::int64_t displacement(unsigned imm7)
{
	return signed_imm7(imm7) * 8;
}

[[nodiscard]] constexpr bool writes_back(Mode mode)
{
	return mode == Mode::post_index || mode == Mode::pre_index;
}

[[nodiscard]] constexpr std::uint64_t adjusted(
	std::uint64_t base, unsigned imm7)
{
	return base + static_cast<std::uint64_t>(displacement(imm7));
}

[[nodiscard]] constexpr std::uint64_t transfer_address(
	std::uint64_t base, Mode mode, unsigned imm7)
{
	return mode == Mode::post_index ? base : adjusted(base, imm7);
}

[[nodiscard]] inline std::uint64_t get(
	std::span<const std::byte> memory, std::size_t offset)
{
	std::uint64_t value = 0;
	for (unsigned byte = 0; byte < 8; ++byte)
		value |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(memory[offset + byte])) <<
			(byte * 8U);
	return value;
}

inline void put(
	std::span<std::byte> memory, std::size_t offset, std::uint64_t value)
{
	for (unsigned byte = 0; byte < 8; ++byte)
		memory[offset + byte] = static_cast<std::byte>(value >> (byte * 8U));
}

inline void fill(std::span<std::byte> memory, unsigned salt)
{
	for (std::size_t index = 0; index < memory.size(); ++index)
		memory[index] = static_cast<std::byte>(
			0x80U | ((index * 37U + salt * 23U) & 0x7fU));
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
	std::string_view name, const Variant &variant, unsigned imm7, unsigned rn,
	unsigned rt, unsigned rt2, const arm64::CpuState &initial,
	std::uint64_t region_address, std::span<const std::byte> initial_memory)
{
	auto expected = initial;
	auto expected_memory =
		std::vector<std::byte>(initial_memory.begin(), initial_memory.end());
	const auto base = atomic::read_register(initial, rn, true);
	const auto address = transfer_address(base, variant.mode, imm7);
	const auto offset = static_cast<std::size_t>(address - region_address);
	if (variant.load) {
		const auto first = get(initial_memory, offset);
		const auto second = get(initial_memory, offset + 8U);
		atomic::write_register(expected, rt, first);
		atomic::write_register(expected, rt2, second);
	} else {
		const auto first = atomic::read_register(initial, rt);
		const auto second = atomic::read_register(initial, rt2);
		put(expected_memory, offset, first);
		put(expected_memory, offset + 8U, second);
	}
	bool writeback = writes_back(variant.mode);
	if (variant.load && rn != 31U && (rn == rt || rn == rt2))
		writeback = false; // This implementation selects WBSUPPRESS.
	if (writeback)
		atomic::write_register(expected, rn, adjusted(base, imm7), true);
	const engine::MemoryRegion memory{
		.address = region_address,
		.initial = initial_memory,
		.expected = std::span<const std::byte>{expected_memory},
	};
	const auto native = atomic::lsui();
	harness.check({
		.name = name,
		.instruction = instruction(
			variant.load, variant.mode, imm7, rn, rt, rt2),
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
	const auto native = atomic::lsui();
	harness.check_illegal(name, value, initial, native);
	counts.add(native, true);
}

} // namespace arm64_base_lsui_pair_test
