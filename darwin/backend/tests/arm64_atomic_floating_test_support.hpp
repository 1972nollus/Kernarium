// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_atomic_engine_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

namespace arm64_atomic_floating_test {

namespace arm64 = uml::backend::arm64;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::backend::tests;

using Page = atomic::Page;

enum class Operation : unsigned {
	add,
	maximum,
	maximum_number,
	minimum,
	minimum_number,
};

inline constexpr std::array operations{
	Operation::add,
	Operation::maximum,
	Operation::maximum_number,
	Operation::minimum,
	Operation::minimum_number,
};

struct Width final {
	unsigned size_code;
	std::size_t bytes;
	std::uint64_t one;
	std::uint64_t two;
	std::uint64_t three;
	std::string_view name;
};

inline constexpr std::array widths{
	Width{0, 2, 0x3f80, 0x4000, 0x4040, "bfloat16"},
	Width{1, 2, 0x3c00, 0x4000, 0x4200, "binary16"},
	Width{2, 4, 0x3f80'0000, 0x4000'0000, 0x4040'0000, "binary32"},
	Width{3, 8, 0x3ff0'0000'0000'0000,
		0x4000'0000'0000'0000, 0x4008'0000'0000'0000, "binary64"},
};

[[noreturn]] inline void fail(std::string_view message)
{
	std::fprintf(stderr, "atomic floating-memory test failure: %.*s\n",
		static_cast<int>(message.size()), message.data());
	std::abort();
}

inline void require(bool condition, std::string_view message)
{
	if (!condition)
		fail(message);
}

[[nodiscard]] constexpr unsigned opcode(Operation operation) noexcept
{
	switch (operation) {
	case Operation::add: return 0U;
	case Operation::maximum: return 4U;
	case Operation::maximum_number: return 6U;
	case Operation::minimum: return 5U;
	case Operation::minimum_number: return 7U;
	}
	return 0U;
}

[[nodiscard]] constexpr std::uint32_t raw(
	unsigned size_code, bool acquire, bool release, bool store_only,
	unsigned encoded_opcode, unsigned rn, unsigned rs, unsigned rt) noexcept
{
	/* Independent transcription of the FEAT_LSFE scalar atomic diagram:
	 * VR=1, o3 selects the store-only form, and opc selects the arithmetic
	 * operation. */
	return 0x3c20'0000U | ((size_code & 3U) << 30) |
		(static_cast<unsigned>(acquire) << 23) |
		(static_cast<unsigned>(release) << 22) | ((rs & 31U) << 16) |
		(static_cast<unsigned>(store_only) << 15) |
		((encoded_opcode & 7U) << 12) | ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] constexpr std::uint32_t instruction(
	unsigned size_code, Operation operation, bool acquire, bool release,
	bool store_only, unsigned rn, unsigned rs, unsigned rt = 32U) noexcept
{
	return raw(size_code, acquire, release, store_only, opcode(operation),
		rn, rs, rt == 32U ? (store_only ? 31U : 4U) : rt);
}

static_assert(instruction(
	0, Operation::add, false, false, false, 2, 3, 4) == 0x3c23'0044U);
static_assert(instruction(
	2, Operation::maximum, true, true, false, 2, 3, 4) == 0xbce3'4044U);
static_assert(instruction(
	3, Operation::minimum_number, false, true, true, 2, 3) ==
	0xfc63'f05fU);

[[nodiscard]] constexpr const Width &width(unsigned size_code)
{
	return widths[size_code & 3U];
}

[[nodiscard]] constexpr std::uint64_t basic_result(
	const Width &shape, Operation operation)
{
	if (operation == Operation::add)
		return shape.three;
	if (operation == Operation::maximum ||
	    operation == Operation::maximum_number)
		return shape.two;
	return shape.one;
}

inline void set_vector(arm64::CpuState &cpu, unsigned reg,
	std::uint64_t bits, std::size_t bytes,
	std::byte upper = std::byte{0xa5})
{
	cpu.vectors[reg].bytes.fill(upper);
	for (std::size_t byte = 0; byte < bytes; ++byte)
		cpu.vectors[reg].bytes[byte] =
			static_cast<std::byte>(bits >> (byte * 8U));
}

[[nodiscard]] inline std::uint64_t vector_bits(
	const arm64::CpuState &cpu, unsigned reg, std::size_t bytes)
{
	std::uint64_t result = 0;
	for (std::size_t byte = 0; byte < bytes; ++byte)
		result |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(cpu.vectors[reg].bytes[byte])) <<
			(byte * 8U);
	return result;
}

inline void fill_page(Page &page, unsigned salt)
{
	for (std::size_t index = 0; index < page.size(); ++index)
		page[index] = static_cast<std::byte>(
			(index * 37U + salt * 53U + 11U) & 0xffU);
}

[[nodiscard]] inline engine::NativeRequirements native()
{
	return engine::NativeRequirements::features(
		uml::backend::ArmFeature::lsfe);
}

struct ExpectedCounts final {
	std::size_t cases{};
	std::size_t jit{};
	std::size_t feature_skips{};

	void add(bool invalid = false)
	{
		++cases;
		if (invalid || engine::host_supports(native()))
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

inline void set_base(
	arm64::CpuState &cpu, unsigned rn, std::uint64_t address)
{
	atomic::write_register(cpu, rn, address, true);
}

inline void check(
	engine::EngineHarness &harness, ExpectedCounts &counts,
	std::string_view name, unsigned size_code, Operation operation,
	bool acquire, bool release, bool store_only, unsigned rn, unsigned rs,
	unsigned rt, std::uint64_t address, std::uint64_t old_value,
	std::uint64_t source_value, std::uint64_t result_value,
	std::span<const engine::AllocationTag> tags = {},
	bool mte_enabled = false, bool tco = false, std::uint32_t fpcr = 0)
{
	const auto &shape = width(size_code);
	Page initial_page{};
	fill_page(initial_page, size_code * 17U + opcode(operation));
	const auto physical_address = mte_enabled && rn != 31U ?
		address & 0x00ff'ffff'ffff'ffffULL : address;
	const auto offset =
		static_cast<std::size_t>(physical_address - atomic::data_page);
	require(offset + shape.bytes <= initial_page.size(),
		"success address outside test page");
	atomic::put(initial_page, offset, old_value, shape.bytes);
	auto expected_page = initial_page;
	atomic::put(expected_page, offset, result_value, shape.bytes);

	auto initial = atomic::state(
		0x1000U + size_code * 257U + opcode(operation) * 31U +
		static_cast<unsigned>(acquire) * 7U +
		static_cast<unsigned>(release) * 3U +
		static_cast<unsigned>(store_only));
	initial.fpcr = fpcr;
	initial.fpsr = 0x0800'009fU;
	initial.mte_enabled = mte_enabled;
	initial.pstate_tco = tco;
	set_base(initial, rn, address);
	if (!store_only && rt != rs)
		set_vector(initial, rt, 0x55aa'33cc'0f0f'f0f0ULL,
			shape.bytes, std::byte{0x5a});
	set_vector(initial, rs, source_value, shape.bytes);

	auto expected = initial;
	if (!store_only) {
		expected.vectors[rt].bytes.fill(std::byte{});
		for (std::size_t byte = 0; byte < shape.bytes; ++byte)
			expected.vectors[rt].bytes[byte] =
				static_cast<std::byte>(old_value >> (byte * 8U));
	}
	const engine::MemoryRegion memory{
		.address = atomic::data_page,
		.initial = initial_page,
		.expected = expected_page,
		.initial_tags = tags,
	};
	harness.check({
		.name = name,
		.instruction = instruction(
			size_code, operation, acquire, release, store_only,
			rn, rs, rt),
		.initial = initial,
		.expected = expected,
		.native = native(),
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
	counts.add();
}

inline void check_basic(
	engine::EngineHarness &harness, ExpectedCounts &counts,
	std::string_view name, unsigned size_code, Operation operation,
	bool acquire, bool release, bool store_only, unsigned rn = 2U,
	unsigned rs = 3U, unsigned rt = 4U,
	std::uint64_t address = atomic::data_address)
{
	const auto &shape = width(size_code);
	check(harness, counts, name, size_code, operation, acquire, release,
		store_only, rn, rs, store_only ? 31U : rt, address, shape.one,
		shape.two, basic_result(shape, operation));
}

inline void check_fault(
	engine::EngineHarness &harness, ExpectedCounts &counts,
	std::string_view name, unsigned size_code, Operation operation,
	bool acquire, bool release, bool store_only, unsigned rn, unsigned rs,
	unsigned rt, std::uint64_t address, std::uint64_t fault_address,
	bool mapped, bool readable, bool writable,
	std::span<const engine::AllocationTag> tags = {},
	bool mte_enabled = false, bool tco = false)
{
	const auto &shape = width(size_code);
	auto initial = atomic::state(
		0x7000U + size_code * 257U + opcode(operation) * 17U);
	initial.fpcr = 2U | (1U << 24) | 0x9f00U;
	initial.fpsr = 0x0800'009fU;
	initial.mte_enabled = mte_enabled;
	initial.pstate_tco = tco;
	set_base(initial, rn, address);
	set_vector(initial, rs, shape.two, shape.bytes);
	if (!store_only && rt != rs)
		set_vector(initial, rt, 0x55aa'33cc'0f0f'f0f0ULL,
			shape.bytes, std::byte{0x5a});

	Page page{};
	fill_page(page, 0x51U + size_code);
	const engine::MemoryRegion memory{
		.address = atomic::data_page,
		.initial = page,
		.expected = page,
		.initial_tags = tags,
		.readable = readable,
		.writable = writable,
	};
	harness.check({
		.name = name,
		.instruction = instruction(
			size_code, operation, acquire, release, store_only,
			rn, rs, store_only ? 31U : rt),
		.initial = initial,
		.expected = initial,
		.event = engine::ExpectedEvent::memory_fault,
		.native = native(),
		.fault = {
			.address = fault_address,
			.access = arm64::MemoryAccess::write,
		},
		.compare_fault = true,
		.memory = mapped ?
			std::span<const engine::MemoryRegion>{&memory, 1} :
			std::span<const engine::MemoryRegion>{},
	});
	counts.add();
}

inline void check_illegal(
	engine::EngineHarness &harness, ExpectedCounts &counts,
	std::string_view name, std::uint32_t value,
	const arm64::CpuState &initial)
{
	harness.check_illegal(name, value, initial, native());
	counts.add(true);
}

} // namespace arm64_atomic_floating_test
