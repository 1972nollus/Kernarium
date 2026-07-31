// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

namespace arm64_simd_pmull_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

inline constexpr std::uint32_t terminal_system_call = 0xd400'0001U;
inline constexpr std::uint64_t data_base =
	engine_tests::engine_test_data_base;
inline constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr std::size_t maximum_program_instructions = 63U;
inline constexpr std::size_t maximum_products_per_program =
	(maximum_program_instructions - 1U) / 2U;

struct Registers final {
	unsigned destination;
	unsigned left;
	unsigned right;
};

struct Variant final {
	bool high;
	unsigned size;
	Registers registers;
};

struct ProgramCoverage final {
	std::size_t programs{};
	std::size_t instructions{};
};

inline constexpr std::array alias_register_cases{
	Registers{0, 1, 2},
	Registers{1, 1, 2},
	Registers{2, 1, 2},
	Registers{0, 1, 1},
	Registers{1, 1, 1},
	Registers{31, 30, 29},
	Registers{31, 31, 30},
	Registers{31, 30, 31},
	Registers{31, 30, 30},
	Registers{31, 31, 31},
};

inline constexpr std::array<std::uint64_t, 16> edge_values{
	std::uint64_t{},
	~std::uint64_t{},
	std::uint64_t{1},
	std::uint64_t{1} << 63U,
	0xaaaa'aaaa'aaaa'aaaaULL,
	0x5555'5555'5555'5555ULL,
	0x0123'4567'89ab'cdefULL,
	0xfedc'ba98'7654'3210ULL,
	0x8000'0000'0000'0001ULL,
	0x7fff'ffff'ffff'ffffULL,
	0x0000'0000'ffff'ffffULL,
	0xffff'ffff'0000'0000ULL,
	0x8080'8080'8080'8080ULL,
	0x0101'0101'0101'0101ULL,
	0x8142'2418'1824'4281ULL,
	0x7e3c'1800'0018'3c7eULL,
};

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

/*
 * Direct transcription of the PMULL/PMULL2 encoding diagram. This is test
 * code only and deliberately does not call the production decoder.
 */
[[nodiscard]] constexpr std::uint32_t instruction(
	const Variant &variant) noexcept
{
	return 0x0e20'e000U |
		(variant.high ? 0x4000'0000U : 0U) |
		((variant.size & 3U) << 22) |
		((variant.registers.right & 31U) << 16) |
		((variant.registers.left & 31U) << 5) |
		(variant.registers.destination & 31U);
}

[[nodiscard]] constexpr bool valid_size(unsigned size) noexcept
{
	return size == 0U || size == 3U;
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements(unsigned size) noexcept
{
	using uml::backend::ArmFeature;
	if (size == 0U)
		return engine_tests::NativeRequirements::features(
			ArmFeature::advanced_simd);
	if (size == 3U)
		return engine_tests::NativeRequirements::features(
			{ArmFeature::advanced_simd, ArmFeature::pmull});
	return engine_tests::NativeRequirements::baseline();
}

[[nodiscard]] inline arm64::CpuState state(std::uint64_t seed = 0)
{
	arm64::CpuState cpu{};
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size();
		     ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed * 67U + reg * 29U + byte * 17U + 3U);
	for (unsigned reg = 0; reg < cpu.registers.size(); ++reg)
		cpu.registers[reg] =
			(seed ^ (static_cast<std::uint64_t>(reg + 1U) *
				0x9e37'79b9'7f4a'7c15ULL)) +
			static_cast<std::uint64_t>(reg) *
				0x0102'0304'0506'0708ULL;
	cpu.sp = seed ^ 0xfeed'face'cafe'bee0ULL;
	cpu.nzcv = static_cast<std::uint32_t>((seed >> 3) & 15U) << 28;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'009fU;
	cpu.fpmr = seed ^ 0x0123'4567'89ab'cdefULL;
	cpu.tls = seed ^ 0xfedc'ba98'7654'3210ULL;
	cpu.set_mte_configuration(true, 0xa55aU);
	cpu.set_mte_generator(true, 0x1357U, 9U,
		seed ^ 0x3141'5926'5358'9793ULL);
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
	cpu.exclusive_address = seed ^ 0x1234'5000ULL;
	cpu.exclusive_key = static_cast<std::uintptr_t>(
		seed ^ 0x2468'1357U);
	cpu.exclusive_value = seed ^ 0x1122'3344'5566'7788ULL;
	cpu.exclusive_value2 = seed ^ 0x8877'6655'4433'2211ULL;
	cpu.exclusive_version = seed ^ 0x1020'3040'5060'7080ULL;
	cpu.exclusive_size = 8;
	return cpu;
}

[[nodiscard]] constexpr std::uint16_t carryless_byte(
	unsigned left, unsigned right) noexcept
{
	std::uint16_t result = 0;
	for (unsigned right_bit = 0; right_bit < 8U; ++right_bit)
		if ((right & (1U << right_bit)) != 0)
			result ^= static_cast<std::uint16_t>(
				(left & 0xffU) << right_bit);
	return result;
}

using WideUnsigned = unsigned __int128;

[[nodiscard]] constexpr WideUnsigned carryless_doubleword(
	std::uint64_t left, std::uint64_t right) noexcept
{
	WideUnsigned result = 0;
	for (unsigned right_bit = 0; right_bit < 64U; ++right_bit)
		if ((right & (std::uint64_t{1} << right_bit)) != 0)
			result ^= static_cast<WideUnsigned>(left) << right_bit;
	return result;
}

static_assert(carryless_byte(0x57U, 0x83U) == 0x2b79U);
static_assert(carryless_byte(0xffU, 0xffU) == 0x5555U);
static_assert(carryless_doubleword(~std::uint64_t{}, 1U) ==
	static_cast<WideUnsigned>(~std::uint64_t{}));
static_assert(carryless_doubleword(
	std::uint64_t{1} << 63U, std::uint64_t{1} << 63U) ==
	(static_cast<WideUnsigned>(1) << 126U));

[[nodiscard]] constexpr std::uint64_t read_doubleword(
	const std::array<std::byte, 16> &bytes, unsigned lane) noexcept
{
	std::uint64_t result = 0;
	for (unsigned byte = 0; byte < 8U; ++byte)
		result |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(bytes[lane * 8U + byte])) <<
			(byte * 8U);
	return result;
}

constexpr void write_doubleword(std::array<std::byte, 16> &bytes,
	unsigned lane, std::uint64_t value) noexcept
{
	for (unsigned byte = 0; byte < 8U; ++byte)
		bytes[lane * 8U + byte] = static_cast<std::byte>(
			value >> (byte * 8U));
}

[[nodiscard]] inline arm64::CpuState edge_state(unsigned pattern)
{
	auto cpu = state(0x100U + pattern);
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned lane = 0; lane < 2U; ++lane)
			write_doubleword(cpu.vectors[reg].bytes, lane,
				edge_values[
					(pattern * 3U + reg * 5U + lane * 7U) %
					edge_values.size()]);
	return cpu;
}

/*
 * Independent specification oracle. Sources are captured before Vd is
 * modified so every architectural alias pattern is handled explicitly.
 */
inline void apply(arm64::CpuState &cpu, const Variant &variant)
{
	require(valid_size(variant.size),
		"PMULL oracle received a reserved size");
	const auto left = cpu.vectors[variant.registers.left].bytes;
	const auto right = cpu.vectors[variant.registers.right].bytes;
	std::array<std::byte, 16> result{};
	const unsigned source_offset = variant.high ? 8U : 0U;
	if (variant.size == 0U) {
		for (unsigned lane = 0; lane < 8U; ++lane) {
			const auto product = carryless_byte(
				std::to_integer<unsigned>(
					left[source_offset + lane]),
				std::to_integer<unsigned>(
					right[source_offset + lane]));
			result[lane * 2U] = static_cast<std::byte>(product);
			result[lane * 2U + 1U] =
				static_cast<std::byte>(product >> 8U);
		}
	} else {
		const auto product = carryless_doubleword(
			read_doubleword(left, variant.high ? 1U : 0U),
			read_doubleword(right, variant.high ? 1U : 0U));
		write_doubleword(result, 0,
			static_cast<std::uint64_t>(product));
		write_doubleword(result, 1,
			static_cast<std::uint64_t>(product >> 64U));
	}
	cpu.vectors[variant.registers.destination].bytes = result;
}

[[nodiscard]] inline arm64::CpuState expected_state(
	const arm64::CpuState &initial, const Variant &variant)
{
	auto expected = initial;
	apply(expected, variant);
	return expected;
}

inline void check_one(engine_tests::EngineHarness &harness,
	std::string_view name, const Variant &variant,
	const arm64::CpuState &initial)
{
	require(valid_size(variant.size),
		"valid PMULL test constructed a reserved size");
	harness.check_one(name, instruction(variant), initial,
		expected_state(initial, variant),
		native_requirements(variant.size));
}

[[nodiscard]] constexpr std::uint32_t store_q(
	unsigned source, unsigned base, unsigned slot) noexcept
{
	return 0x3d80'0000U | ((slot & 0xfffU) << 10) |
		((base & 31U) << 5) | (source & 31U);
}

class ProgramBatcher final {
public:
	ProgramBatcher(engine_tests::EngineHarness &harness,
		std::string_view name, unsigned size, bool high,
		std::uint64_t seed = 0)
		: harness_{harness}, name_{name}, size_{size}, high_{high},
		  seed_{seed}
	{
		require(valid_size(size_),
			"PMULL program batcher received a reserved size");
		variants_.reserve(maximum_products_per_program);
	}

	void add(Registers registers)
	{
		variants_.push_back({high_, size_, registers});
		if (variants_.size() == maximum_products_per_program)
			flush();
	}

	void finish()
	{
		flush();
	}

	[[nodiscard]] ProgramCoverage coverage() const noexcept
	{
		return coverage_;
	}

private:
	void flush()
	{
		if (variants_.empty())
			return;
		std::vector<engine_tests::ProgramWord> code;
		code.reserve(variants_.size() * 2U + 1U);
		auto initial = state(++seed_);
		initial.registers[0] = data_base;
		/* The observation stores are permitted to clear a local monitor.
		 * Start with no reservation so that this test remains about the
		 * multiply result rather than an implementation-defined side effect.
		 */
		initial.exclusive_size = 0;
		auto expected = initial;
		std::array<std::byte, page_size> initial_memory{};
		std::array<std::byte, page_size> expected_memory{};
		for (std::size_t byte = 0; byte < page_size; ++byte)
			initial_memory[byte] = static_cast<std::byte>(
				seed_ * 13U + byte * 29U + 7U);
		expected_memory = initial_memory;
		for (std::size_t index = 0; index < variants_.size(); ++index) {
			code.push_back({
				.offset = code.size() * sizeof(std::uint32_t),
				.instruction = instruction(variants_[index]),
			});
			apply(expected, variants_[index]);
			code.push_back({
				.offset = code.size() * sizeof(std::uint32_t),
				.instruction = store_q(
					variants_[index].registers.destination,
					0U, static_cast<unsigned>(index)),
			});
			const auto &saved =
				expected.vectors[
					variants_[index].registers.destination].bytes;
			for (std::size_t byte = 0; byte < saved.size(); ++byte)
				expected_memory[index * saved.size() + byte] =
					saved[byte];
		}
		code.push_back({
			.offset = code.size() * sizeof(std::uint32_t),
			.instruction = terminal_system_call,
		});
		expected.pc = engine_tests::engine_test_code_base +
			code.size() * sizeof(std::uint32_t);
		const std::array memory{
			engine_tests::MemoryRegion{
				.address = data_base,
				.initial =
					std::span<const std::byte>{initial_memory},
				.expected =
					std::span<const std::byte>{expected_memory},
			},
		};
		harness_.check_program({
			.name = name_,
			.code =
				std::span<const engine_tests::ProgramWord>{code},
			.initial = initial,
			.expected = expected,
			.native = native_requirements(size_),
			.memory =
				std::span<const engine_tests::MemoryRegion>{memory},
		});
		++coverage_.programs;
		coverage_.instructions += variants_.size();
		variants_.clear();
	}

	engine_tests::EngineHarness &harness_;
	std::string_view name_;
	unsigned size_;
	bool high_;
	std::uint64_t seed_;
	std::vector<Variant> variants_;
	ProgramCoverage coverage_;
};

inline void require_engine_counts(
	const engine_tests::EngineHarness &harness,
	std::size_t size_zero_cases, std::size_t size_three_cases,
	std::size_t invalid_cases = 0U)
{
	const auto &counts = harness.counts();
	const auto valid_cases = size_zero_cases + size_three_cases;
	const auto total = valid_cases + invalid_cases;
	const auto baseline_supported = engine_tests::host_supports(
		native_requirements(0U));
	const auto extended_supported = engine_tests::host_supports(
		native_requirements(3U));
	const auto expected_host_skips =
		(baseline_supported ? 0U : size_zero_cases) +
		(extended_supported ? 0U : size_three_cases);
	require(counts.interpreter == total,
		"PMULL interpreter case count is incomplete");
	require(counts.gadgets == total,
		"PMULL gadget case count is incomplete");
	require(counts.jit + counts.jit_skipped == total,
		"PMULL JIT case accounting is incomplete");
	require(counts.jit == total - expected_host_skips,
		"PMULL JIT executed an unexpected number of cases");
	require(counts.jit_skipped == expected_host_skips,
		"PMULL JIT skipped an unexpected case");
	require(counts.jit_skipped_host_feature == expected_host_skips,
		"PMULL JIT host-feature skip count is wrong");
	require(counts.jit_unavailable == 0U,
		"PMULL JIT family is unavailable");
}

} // namespace arm64_simd_pmull_test
