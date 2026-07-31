// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "arm64_system_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

namespace arm64_hint_test {

namespace arm64 = uml::backend::arm64;
namespace engine_tests = uml::backend::tests;

inline constexpr std::uint32_t speculation_barrier_instruction =
	0xd503'30ffU;

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

[[nodiscard]] constexpr std::uint32_t primary_instruction(
	unsigned immediate)
{
	return 0xd503'201fU | ((immediate & 0x7fU) << 5);
}

[[nodiscard]] constexpr std::uint32_t extended_instruction(
	unsigned immediate)
{
	return 0xd500'2000U |
		(((immediate >> 15) & 1U) << 21) |
		(((immediate >> 12) & 7U) << 16) |
		(((immediate >> 5) & 0x7fU) << 5) |
		(immediate & 0x1fU);
}

[[nodiscard]] constexpr std::uint32_t gsb_instruction(
	unsigned operation, unsigned transfer)
{
	return 0xd508'c000U | ((operation & 7U) << 5) |
		(transfer & 31U);
}

[[nodiscard]] constexpr bool is_primary_hint(unsigned immediate)
{
	return immediate >= 12'319U && immediate <= 16'383U &&
		((immediate - 12'319U) % 32U) == 0;
}

[[nodiscard]] constexpr bool is_allocated_primary_hint(unsigned immediate)
{
	if (immediate <= 7U || (immediate >= 16U && immediate <= 20U) ||
	    (immediate >= 24U && immediate <= 32U) || immediate == 34U ||
	    immediate == 36U || (immediate >= 38U && immediate <= 40U) ||
	    (immediate >= 48U && immediate <= 52U))
		return true;
	return immediate == 8U || immediate == 10U ||
		immediate == 12U || immediate == 14U ||
		immediate == 22U;
}

/*
 * These primary-HINT encodings are delegated by the architecture to the
 * pointer-authentication instruction descriptions. They have architectural
 * register effects and are covered by that family's stateful tests rather
 * than by the stateless HINT/event family.
 */
[[nodiscard]] constexpr bool is_delegated_stateful_hint(unsigned immediate)
{
	return immediate == 7U || immediate == 8U || immediate == 10U ||
		immediate == 12U || immediate == 14U ||
		(immediate >= 24U && immediate <= 31U) ||
		immediate == 39U;
}

[[nodiscard]] constexpr bool is_owned_primary_hint(unsigned immediate)
{
	return immediate < 128U &&
		immediate != 40U &&
		immediate != 48U &&
		immediate != 49U &&
		immediate != 52U &&
		!is_delegated_stateful_hint(immediate);
}

[[nodiscard]] inline engine_tests::NativeRequirements
primary_requirements(unsigned immediate)
{
	using uml::backend::ArmFeature;
	switch (immediate) {
	case 6:
		return engine_tests::NativeRequirements::features(
			ArmFeature::dgh);
	case 7:
	case 8:
	case 10:
	case 12:
	case 14:
	case 24:
	case 25:
	case 26:
	case 27:
	case 28:
	case 29:
	case 30:
	case 31:
		return engine_tests::NativeRequirements::baseline();
	case 16:
		return engine_tests::NativeRequirements::features(
			ArmFeature::ras);
	case 17:
		return engine_tests::NativeRequirements::features(
			ArmFeature::spe);
	case 18:
		return engine_tests::NativeRequirements::features(
			ArmFeature::trf);
	case 19:
		return engine_tests::NativeRequirements::features(
			ArmFeature::gcs);
	case 22:
		return engine_tests::NativeRequirements::features(
			ArmFeature::clrbhb);
	case 32:
	case 34:
	case 36:
	case 38:
		return engine_tests::NativeRequirements::features(
			ArmFeature::bti);
	case 39:
		return engine_tests::NativeRequirements::features(
			ArmFeature::pauth_lr);
	case 40:
		return engine_tests::NativeRequirements::features(
			ArmFeature::chk);
	case 48:
	case 49:
		return engine_tests::NativeRequirements::features(
			ArmFeature::pcdphint);
	case 50:
	case 51:
	case 52:
		return engine_tests::NativeRequirements::features(
			ArmFeature::cmh);
	default:
		return engine_tests::NativeRequirements::baseline();
	}
}

[[nodiscard]] inline arm64::CpuState seeded_state(std::uint64_t seed)
{
	auto cpu = arm64_system_test::state();
	for (unsigned index = 0; index < cpu.registers.size(); ++index)
		cpu.registers[index] ^=
			seed + 0x9e37'79b9'7f4a'7c15ULL * (index + 1U);
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size();
		     ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed + reg * 37U + byte * 19U);
	cpu.sp ^= seed;
	cpu.nzcv = static_cast<std::uint32_t>((seed >> 7) & 15U) << 28U;
	cpu.fpcr ^= static_cast<std::uint32_t>(seed) & 0x0040'0000U;
	cpu.fpsr ^= static_cast<std::uint32_t>(seed) & 0x0800'009fU;
	cpu.fpmr ^= seed & 0x0000'003f'ff7f'c1ffULL;
	cpu.tls ^= seed;
	cpu.exclusive_address ^= seed;
	cpu.exclusive_key ^= static_cast<std::uintptr_t>(seed);
	cpu.exclusive_value ^= seed;
	cpu.exclusive_value2 ^= ~seed;
	cpu.exclusive_version += seed;
	cpu.exclusive_size = 1U << ((seed >> 3) & 3U);
	/*
	 * CHKFEAT only clears selected enabled-feature bits. Keeping selector
	 * bit zero clear makes HINT #40's architectural result independent of
	 * the host's GCS enablement, while every other state field remains noisy.
	 */
	cpu.registers[16] &= ~std::uint64_t{1};
	return cpu;
}

[[nodiscard]] inline arm64::CpuState stateless_oracle(
	std::uint32_t instruction, const arm64::CpuState &initial)
{
	if ((instruction & 0xffff'f01fU) == 0xd503'201fU) {
		const unsigned immediate = (instruction >> 5) & 0x7fU;
		require(is_owned_primary_hint(immediate),
			"stateless HINT oracle received a delegated alias");
		if (immediate == 40U)
			require((initial.registers[16] & 1U) == 0,
				"CHKFEAT-invariant HINT case selected GCS");
		return initial;
	}
	if (instruction == speculation_barrier_instruction)
		return initial;
	fail("stateless HINT oracle received an unrelated instruction");
}

class Harness final {
public:
	void check(std::string_view name, std::uint32_t instruction,
		engine_tests::NativeRequirements native)
	{
		const auto initial = seeded_state(instruction);
		check(name, instruction, initial, initial, native);
	}

	void check(std::string_view name, std::uint32_t instruction,
		const arm64::CpuState &initial,
		const arm64::CpuState &expected,
		engine_tests::NativeRequirements native)
	{
		engines_.check_one(name, instruction, initial, expected, native);
	}

	void check_stateless(std::string_view name,
		std::uint32_t instruction, const arm64::CpuState &initial,
		engine_tests::NativeRequirements native)
	{
		check(name, instruction, initial,
			stateless_oracle(instruction, initial), native);
	}

	void check_profile_illegal(std::string_view name,
		std::uint32_t instruction, const arm64::CpuState &initial,
		engine_tests::NativeRequirements native)
	{
		engines_.check({
			.name = name,
			.instruction = instruction,
			.initial = initial,
			.expected = initial,
			.event = engine_tests::ExpectedEvent::illegal_instruction,
			.native = native,
		});
	}

	void check_reserved(std::string_view name,
		std::uint32_t instruction, const arm64::CpuState &initial)
	{
		engines_.check_illegal(name, instruction, initial);
	}

	void check_program(std::string_view name,
		std::span<const engine_tests::ProgramWord> code,
		const arm64::CpuState &initial,
		const arm64::CpuState &expected,
		engine_tests::NativeRequirements native)
	{
		engines_.check_program({
			.name = name,
			.code = code,
			.initial = initial,
			.expected = expected,
			.native = native,
		});
	}

	[[nodiscard]] const engine_tests::EngineTestCounts &counts() const
	{
		return engines_.counts();
	}

	void print_summary(std::string_view name) const
	{
		engines_.print_summary(name);
	}

private:
	engine_tests::EngineHarness engines_;
};

struct ExpectedCounts final {
	std::size_t cases{};
	std::size_t jit{};
	std::size_t jit_feature_skips{};

	void add(engine_tests::NativeRequirements native,
		bool invalid_encoding = false)
	{
		++cases;
		if (invalid_encoding || engine_tests::host_supports(native))
			++jit;
		else
			++jit_feature_skips;
	}
};

inline void require_counts(const Harness &harness,
	const ExpectedCounts &expected)
{
	const auto &actual = harness.counts();
	require(actual.interpreter == expected.cases,
		"HINT interpreter did not run every case");
	require(actual.gadgets == expected.cases,
		"HINT gadgets did not run every case");
	require(actual.jit == expected.jit,
		"HINT JIT execution count did not match exact feature gates");
	require(actual.jit_skipped_host_feature ==
			expected.jit_feature_skips,
		"HINT JIT host-feature skip count is incorrect");
	require(actual.jit_skipped ==
			expected.jit_feature_skips,
		"HINT JIT had a non-feature skip");
	require(actual.jit_unavailable == 0U,
		"HINT JIT was unexpectedly unavailable");
}

} // namespace arm64_hint_test
