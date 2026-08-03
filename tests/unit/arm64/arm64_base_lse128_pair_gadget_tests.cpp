// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_lse128_pair_test_support.hpp"

#include <array>
#include <span>

namespace pair128 = arm64_base_lse128_pair_test;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;

namespace {

constexpr std::array operations{
	pair128::Operation::clear,
	pair128::Operation::set,
	pair128::Operation::swap,
};

void specialized_multi_instruction_blocks(
	engine::EngineHarness &harness, pair128::ExpectedCounts &counts)
{
	for (unsigned ordering = 0; ordering < 4; ++ordering) {
		const bool acquire = (ordering & 2U) != 0U;
		const bool release = (ordering & 1U) != 0U;
		auto initial = atomic::state(0x7000U + ordering);
		atomic::Page initial_page{};
		for (std::size_t index = 0; index < initial_page.size(); ++index)
			initial_page[index] =
				static_cast<std::byte>((index * 43U + ordering * 17U) &
					0xffU);
		auto expected_page = initial_page;
		auto expected = initial;
		std::array<engine::ProgramWord, 4> code{};
		for (unsigned index = 0; index < operations.size(); ++index) {
			const unsigned first = 2U + index * 2U;
			const unsigned second = first + 1U;
			const unsigned base = 10U + index;
			const std::size_t offset = atomic::data_offset + index * 32U;
			const std::uint64_t address = atomic::data_page + offset;
			const pair128::Pair old_value{
				pair128::observed[0] ^ (std::uint64_t{index + 1U} << 52),
				pair128::observed[1] ^ (std::uint64_t{index + 1U} << 48),
			};
			const pair128::Pair source{
				pair128::operand[0] ^ (std::uint64_t{index + 1U} << 44),
				pair128::operand[1] ^ (std::uint64_t{index + 1U} << 40),
			};
			pair128::write_pair(initial, first, second, source);
			atomic::write_register(initial, base, address, true);
			pair128::put_pair(initial_page, offset, old_value);
			pair128::put_pair(expected_page, offset,
				pair128::oracle(operations[index], old_value, source));
			pair128::write_pair(expected, first, second, old_value);
			atomic::write_register(expected, base, address, true);
			code[index] = {
				.offset = index * sizeof(std::uint32_t),
				.instruction = pair128::instruction(operations[index],
					acquire, release, base, first, second),
			};
		}
		code[3] = {
			.offset = 3U * sizeof(std::uint32_t),
			.instruction = atomic::terminal_system_call,
		};
		expected.pc = engine::engine_test_code_base +
			4U * sizeof(std::uint32_t);
		const auto memory = atomic::region(initial_page, expected_page);
		const auto native = atomic::lse128();
		harness.check_program({
			.name = "translation-specialized LSE128 operation block",
			.code = std::span<const engine::ProgramWord>{code},
			.initial = initial,
			.expected = expected,
			.native = native,
			.memory = std::span<const engine::MemoryRegion>{&memory, 1},
		});
		counts.add(native);
	}
}

void equal_value_swap_invalidates_reservation(
	engine::EngineHarness &harness, pair128::ExpectedCounts &counts)
{
	atomic::Page page{};
	pair128::put_pair(page, atomic::data_offset, pair128::observed);
	const auto memory = atomic::region(page, page);
	const std::array code{
		engine::ProgramWord{
			0, atomic::scalar_exclusive(3, true, false, 2, 5)},
		engine::ProgramWord{
			4, pair128::instruction(
				pair128::Operation::swap, false, false, 2, 3, 4)},
		engine::ProgramWord{
			8, atomic::scalar_exclusive(3, false, false, 2, 7, 6)},
		engine::ProgramWord{12, atomic::clrex(3)},
		engine::ProgramWord{16, atomic::terminal_system_call},
	};
	auto initial = atomic::state(0x7100);
	initial.registers[2] = atomic::data_address;
	pair128::write_pair(initial, 3, 4, pair128::observed);
	initial.registers[7] = ~pair128::observed[0];
	auto expected = initial;
	expected.registers[5] = pair128::observed[0];
	pair128::write_pair(expected, 3, 4, pair128::observed);
	expected.registers[6] = 1;
	atomic::clear_monitor(expected);
	expected.pc = engine::engine_test_code_base +
		5U * sizeof(std::uint32_t);
	const auto native = atomic::lse128();
	harness.check_program({
		.name = "equal-value SWPP invalidates reservation",
		.code = std::span<const engine::ProgramWord>{code},
		.initial = initial,
		.expected = expected,
		.native = native,
		.memory = std::span<const engine::MemoryRegion>{&memory, 1},
	});
	counts.add(native);
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	pair128::ExpectedCounts counts;
	specialized_multi_instruction_blocks(harness, counts);
	equal_value_swap_invalidates_reservation(harness, counts);
	pair128::require_counts(harness, counts);
	harness.print_summary("arm64-base-lse128-pair-gadget");
}
