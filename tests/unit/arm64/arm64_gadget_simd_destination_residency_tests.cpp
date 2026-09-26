// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_three_same_test_support.hpp"

#include "cpu/arm64/gadgets/gadget_symbols.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory_resource>
#include <source_location>
#include <span>

namespace test = arm64_simd_three_same_test;
namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
namespace selection = gadgets::selection;

namespace {

using Gadget = void (*)();

void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (condition)
		return;
	std::fprintf(stderr, "require failed at %s:%u\n",
		location.file_name(), location.line());
	std::abort();
}

[[nodiscard]] std::uintptr_t address(Gadget gadget) noexcept
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] constexpr std::uint32_t mla(
	unsigned destination, unsigned left = 8U, unsigned right = 9U,
	bool q = true, unsigned size = 2U)
{
	return test::instruction(false, q, false, size, 18,
		{destination, left, right});
}

void exact_run_shape()
{
	constexpr std::uint64_t base = 0x91'0000;
	constexpr std::array run{
		mla(0), mla(0), mla(0), mla(0),
	};
	const auto built = gadgets::Builder{}.build(
		base, run, {}, std::pmr::get_default_resource());
	const auto words = built.program.words();
	require(built.complete);
	require(built.program.instruction_count() == run.size());

	require(words.size() == 14U);
	require(words[0] == address(
		selection::uml_arm64_gadget_simd_destination_resident_load));
	for (std::size_t index = 0; index < run.size(); ++index)
		require(words[2U + index * 2U] == address(selection::
			uml_arm64_gadget_simd_destination_resident_mla_4s));
	require(words[10] == address(
		selection::uml_arm64_gadget_simd_destination_resident_flush));
	require(words[12] == address(selection::uml_arm64_gadget_interrupt));
}

void unsafe_or_short_runs_use_ordinary_gadgets()
{
	constexpr std::uint64_t base = 0x92'0000;
	constexpr std::array short_run{mla(0), mla(0), mla(0)};
	constexpr std::array aliased{
		mla(0, 0, 9), mla(0, 0, 9), mla(0, 0, 9), mla(0, 0, 9),
	};
	constexpr std::array outside_bank{
		mla(8, 16, 17), mla(8, 16, 17),
		mla(8, 16, 17), mla(8, 16, 17),
	};
	for (const auto instructions : {
		std::span<const std::uint32_t>{short_run},
		std::span<const std::uint32_t>{aliased},
		std::span<const std::uint32_t>{outside_bank},
	}) {
		const auto built = gadgets::Builder{}.build(
			base, instructions, {}, std::pmr::get_default_resource());
		require(built.complete);
		require(built.program.words().front() == address(
			selection::uml_arm64_gadget_simd_three_same_mla_4s));
	}
}

void dirty_destination_is_flushed_before_linking()
{
	constexpr std::uint64_t base = 0x93'0000;
	constexpr std::array run{
		mla(0), mla(0), mla(0), mla(0),
	};
	const gadgets::BlockLink target{};
	const auto built = gadgets::Builder{}.build(base, run,
		[&](std::uint64_t) { return &target; },
		std::pmr::get_default_resource());
	const auto words = built.program.words();
	require(words[10] == address(
		selection::uml_arm64_gadget_simd_destination_resident_flush));
	require(words[12] == address(selection::uml_arm64_gadget_branch_linked));
}

void runtime_result_is_visible_to_the_next_instruction()
{
	constexpr std::uint32_t svc = 0xd400'0001U;
	const auto native = test::native_requirements();
	const std::array code{
		test::engine_tests::ProgramWord{0, mla(0)},
		test::engine_tests::ProgramWord{4, mla(0)},
		test::engine_tests::ProgramWord{8, mla(0)},
		test::engine_tests::ProgramWord{12, mla(0)},
		test::engine_tests::ProgramWord{
			16, test::instruction(false, true, false, 2, 16, {1, 0, 8})},
		test::engine_tests::ProgramWord{20, svc},
	};
	auto initial = test::state(7);
	auto expected = initial;
	for (unsigned iteration = 0; iteration < 4U; ++iteration)
		expected = test::expected_state(
			expected, false, true, false, 2, 18, {0, 8, 9});
	expected = test::expected_state(
		expected, false, true, false, 2, 16, {1, 0, 8});
	expected.pc = test::engine_tests::engine_test_code_base + 24U;

	test::engine_tests::EngineHarness harness;
	harness.check_program({
		.name = "gadget-simd-destination-residency-runtime",
		.code = code,
		.initial = initial,
		.expected = expected,
		.native = native,
		.expected_instructions = code.size(),
	});
	harness.print_summary("arm64-gadget-simd-destination-residency");
}

} // namespace

int main()
{
	exact_run_shape();
	unsafe_or_short_runs_use_ordinary_gadgets();
	dirty_destination_is_flushed_before_linking();
	runtime_result_is_visible_to_the_next_instruction();
	return 0;
}
