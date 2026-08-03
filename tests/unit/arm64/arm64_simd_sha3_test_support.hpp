// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

namespace arm64_simd_sha3_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

enum class Operation : unsigned {
	three_way_exclusive,
	clear_and_exclusive,
	rotate_one_and_exclusive,
	exclusive_and_rotate,
};

struct Registers final {
	unsigned destination;
	unsigned first;
	unsigned second;
	unsigned third;
};

struct Variant final {
	Operation operation;
	Registers registers;
	unsigned rotate{};
};

struct ProgramCoverage final {
	std::size_t programs{};
	std::size_t instructions{};
};

inline constexpr std::size_t maximum_program_instructions = 63U;
inline constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

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

[[nodiscard]] constexpr bool four_sources(Operation operation) noexcept
{
	return operation == Operation::three_way_exclusive ||
		operation == Operation::clear_and_exclusive;
}

[[nodiscard]] constexpr std::string_view operation_name(
	Operation operation) noexcept
{
	switch (operation) {
	case Operation::three_way_exclusive: return "eor3";
	case Operation::clear_and_exclusive: return "bcax";
	case Operation::rotate_one_and_exclusive: return "rax1";
	case Operation::exclusive_and_rotate: return "xar";
	}
	return "simd-logical-rotation";
}

/*
 * Direct transcription of the four encoding diagrams. The oracle below does
 * not include or call production decode or execution helpers.
 */
[[nodiscard]] constexpr std::uint32_t instruction(
	const Variant &variant) noexcept
{
	const auto &registers = variant.registers;
	std::uint32_t result;
	switch (variant.operation) {
	case Operation::three_way_exclusive:
		result = 0xce00'0000U |
			((registers.third & 31U) << 10);
		break;
	case Operation::clear_and_exclusive:
		result = 0xce20'0000U |
			((registers.third & 31U) << 10);
		break;
	case Operation::rotate_one_and_exclusive:
		result = 0xce60'8c00U;
		break;
	case Operation::exclusive_and_rotate:
		result = 0xce80'0000U |
			((variant.rotate & 63U) << 10);
		break;
	}
	return result | ((registers.second & 31U) << 16) |
		((registers.first & 31U) << 5) |
		(registers.destination & 31U);
}

[[nodiscard]] inline engine_tests::NativeRequirements
native_requirements() noexcept
{
	using ::uml::cpu::ArmFeature;
	return engine_tests::NativeRequirements::features(
		{ArmFeature::advanced_simd, ArmFeature::sha3});
}

[[nodiscard]] inline arm64::CpuState state(unsigned seed = 0)
{
	arm64::CpuState cpu{};
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size();
		     ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed * 67U + reg * 29U + byte * 17U + 3U);
	for (unsigned reg = 0; reg < cpu.registers.size(); ++reg)
		cpu.registers[reg] = 0x1020'3040'5060'7080ULL ^
			(static_cast<std::uint64_t>(reg + seed) *
			 0x0101'0101'0101'0101ULL);
	cpu.sp = 0xfeed'face'cafe'bee0ULL;
	cpu.nzcv = 0xa000'0000U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = 0x0800'001fU;
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0xfedc'ba98'7654'3210ULL;
	cpu.set_mte_configuration(true, 0xa55aU);
	cpu.set_mte_generator(true, 0x1357U, 9U,
		0x3141'5926'5358'9793ULL);
	cpu.cpa_enabled = true;
	cpu.cpa_multiply_enabled = true;
	cpu.accdata = 0x89ab'cdefU;
	cpu.pstate_ssbs = true;
	cpu.pstate_dit = true;
	cpu.pstate_tco = true;
	cpu.pstate_sm = true;
	cpu.pstate_za = true;
	cpu.pstate_daif_access = true;
	cpu.pstate_daif = 0xbU;
	cpu.exclusive_address = 0x1234'5000ULL;
	cpu.exclusive_key = 0x2468U;
	cpu.exclusive_value = 0x1122'3344'5566'7788ULL;
	cpu.exclusive_value2 = 0x8877'6655'4433'2211ULL;
	cpu.exclusive_version = 0x1020'3040'5060'7080ULL;
	cpu.exclusive_size = 8;
	return cpu;
}

[[nodiscard]] inline std::uint64_t read_lane(
	const std::array<std::byte, 16> &bytes, unsigned lane) noexcept
{
	std::uint64_t result = 0;
	for (unsigned byte = 0; byte < 8U; ++byte)
		result |= static_cast<std::uint64_t>(
			std::to_integer<unsigned>(bytes[lane * 8U + byte]))
			<< (byte * 8U);
	return result;
}

inline void write_lane(std::array<std::byte, 16> &bytes, unsigned lane,
	std::uint64_t value) noexcept
{
	for (unsigned byte = 0; byte < 8U; ++byte)
		bytes[lane * 8U + byte] =
			static_cast<std::byte>(value >> (byte * 8U));
}

inline void apply(arm64::CpuState &cpu, const Variant &variant)
{
	const auto first = cpu.vectors[variant.registers.first].bytes;
	const auto second = cpu.vectors[variant.registers.second].bytes;
	const auto third = cpu.vectors[variant.registers.third].bytes;
	std::array<std::byte, 16> result{};
	switch (variant.operation) {
	case Operation::three_way_exclusive:
		for (unsigned byte = 0; byte < result.size(); ++byte)
			result[byte] = first[byte] ^ second[byte] ^ third[byte];
		break;
	case Operation::clear_and_exclusive:
		for (unsigned byte = 0; byte < result.size(); ++byte)
			result[byte] =
				first[byte] ^ (second[byte] & ~third[byte]);
		break;
	case Operation::rotate_one_and_exclusive:
		for (unsigned lane = 0; lane < 2U; ++lane)
			write_lane(result, lane,
				read_lane(first, lane) ^
					std::rotl(read_lane(second, lane), 1));
		break;
	case Operation::exclusive_and_rotate:
		for (unsigned lane = 0; lane < 2U; ++lane)
			write_lane(result, lane,
				std::rotr(read_lane(first, lane) ^
						read_lane(second, lane),
					static_cast<int>(variant.rotate & 63U)));
		break;
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
	const Variant &variant, const arm64::CpuState &initial)
{
	harness.check_one(operation_name(variant.operation),
		instruction(variant), initial, expected_state(initial, variant),
		native_requirements());
}

class ProgramBatcher final {
public:
	ProgramBatcher(engine_tests::EngineHarness &harness,
		std::string_view name, unsigned seed = 0)
		: harness_{harness}, name_{name}, seed_{seed}
	{
		variants_.reserve(maximum_program_instructions);
	}

	void add(const Variant &variant)
	{
		variants_.push_back(variant);
		if (variants_.size() == maximum_program_instructions)
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
		code.reserve(variants_.size() + 1U);
		auto initial = state(++seed_);
		auto expected = initial;
		for (std::size_t index = 0; index < variants_.size(); ++index) {
			code.push_back({
				.offset = index * sizeof(std::uint32_t),
				.instruction = instruction(variants_[index]),
			});
			apply(expected, variants_[index]);
		}
		code.push_back({
			.offset = variants_.size() * sizeof(std::uint32_t),
			.instruction = terminal_system_call,
		});
		expected.pc = engine_tests::engine_test_code_base +
			code.size() * sizeof(std::uint32_t);
		harness_.check_program({
			.name = name_,
			.code = std::span<const engine_tests::ProgramWord>{code},
			.initial = initial,
			.expected = expected,
			.native = native_requirements(),
		});
		++coverage_.programs;
		coverage_.instructions += variants_.size();
		variants_.clear();
	}

	engine_tests::EngineHarness &harness_;
	std::string_view name_;
	unsigned seed_;
	std::vector<Variant> variants_;
	ProgramCoverage coverage_;
};

inline void require_engine_counts(
	const engine_tests::EngineHarness &harness,
	std::size_t valid_cases, std::size_t invalid_cases = 0U)
{
	const auto &counts = harness.counts();
	const auto total = valid_cases + invalid_cases;
	const auto expected_host_skips =
		engine_tests::host_supports(native_requirements()) ?
			0U : valid_cases;
	require(counts.interpreter == total,
		"SIMD logical/rotation interpreter case count is incomplete");
	require(counts.gadgets == total,
		"SIMD logical/rotation gadget case count is incomplete");
	require(counts.jit + counts.jit_skipped == total,
		"SIMD logical/rotation JIT case accounting is incomplete");
	require(counts.jit_skipped == expected_host_skips,
		"SIMD logical/rotation JIT skipped an unexpected case");
	require(counts.jit_skipped_host_feature == expected_host_skips,
		"SIMD logical/rotation host-feature skip count is wrong");
	require(counts.jit_unavailable == 0U,
		"SIMD logical/rotation JIT family is unavailable");
}

} // namespace arm64_simd_sha3_test
