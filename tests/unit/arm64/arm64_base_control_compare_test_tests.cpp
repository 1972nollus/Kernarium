// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_control_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

namespace uml::emulator::tests::base_control {
namespace {

namespace gadgets = arm64::gadgets;

constexpr std::array<std::int64_t, 5> compare_displacements{
	-1'048'576, -4, 0, 8, 1'048'572,
};
constexpr std::array<std::int64_t, 5> test_displacements{
	-32'768, -4, 0, 8, 32'764,
};
constexpr std::array<std::uint64_t, 8> compare_values{
	0,
	1,
	0x7fff'ffffU,
	0x8000'0000U,
	0xffff'ffffU,
	0x1'0000'0000ULL,
	0x8000'0000'0000'0000ULL,
	0xffff'ffff'ffff'ffffULL,
};

[[nodiscard]] constexpr std::uint64_t register_value(
	const arm64::CpuState &cpu, unsigned source) noexcept
{
	return source == 31U ? 0U : cpu.registers[source];
}

void compare_semantics(Harness &harness)
{
	for (const bool wide : {false, true})
		for (const bool nonzero : {false, true})
			for (unsigned source = 0; source < 32; ++source)
				for (unsigned value_index = 0;
				     value_index < compare_values.size();
				     ++value_index)
					for (const auto displacement :
					     compare_displacements) {
						const auto instruction =
							encode_compare_branch(
								wide, nonzero,
								displacement,
								source);
						auto initial = seeded_state(
							0x1400'0000'0000'0000ULL ^
							(static_cast<std::uint64_t>(
								instruction) << 13) ^
							value_index);
						if (source != 31U)
							initial.registers[source] =
								compare_values[
									value_index];
						const auto mask = wide ?
							~std::uint64_t{0} :
							std::uint64_t{
								0xffff'ffffU};
						const auto value =
							register_value(initial,
								source) &
							mask;
						const bool take = nonzero ?
							value != 0 : value == 0;
						const auto next_pc = take ?
							static_cast<std::uint64_t>(
								static_cast<
									std::int64_t>(
									branch_pc) +
								displacement) :
							branch_pc + 4;
						char name[176];
						std::snprintf(name, sizeof(name),
							"%s %c%u value=%#llx "
							"displacement=%lld",
							nonzero ? "CBNZ" :
								"CBZ",
							wide ? 'x' : 'w',
							source,
							static_cast<
								unsigned long long>(
								value),
							static_cast<long long>(
								displacement));
						harness.check_branch(
							name, instruction,
							initial, initial,
							next_pc);
					}
}

void test_semantics(Harness &harness)
{
	for (const bool nonzero : {false, true})
		for (unsigned bit = 0; bit < 64; ++bit)
			for (unsigned source = 0; source < 32; ++source)
				for (const bool set : {false, true})
					for (const auto displacement :
					     test_displacements) {
						const auto instruction =
							encode_test_branch(
								nonzero, bit,
								displacement,
								source);
						auto initial = seeded_state(
							0x1500'0000'0000'0000ULL ^
							(static_cast<std::uint64_t>(
								instruction) << 15) ^
							static_cast<std::uint64_t>(
								set));
						if (source != 31U) {
							const auto unrelated =
								std::uint64_t{1} <<
								((bit + 17U) &
									63U);
							initial.registers[source] =
								unrelated |
								(set ?
									std::uint64_t{
										1} <<
										bit :
									0U);
						}
						const bool actual_set =
							((register_value(initial,
								source) >>
								bit) &
							 1U) != 0;
						const bool take = nonzero ?
							actual_set : !actual_set;
						const auto next_pc = take ?
							static_cast<std::uint64_t>(
								static_cast<
									std::int64_t>(
									branch_pc) +
								displacement) :
							branch_pc + 4;
						char name[176];
						std::snprintf(name, sizeof(name),
							"%s x%u bit=%u set=%u "
							"displacement=%lld",
							nonzero ? "TBNZ" :
								"TBZ",
							source, bit,
							static_cast<unsigned>(
								actual_set),
							static_cast<long long>(
								displacement));
						harness.check_branch(
							name, instruction,
							initial, initial,
							next_pc);
					}
}

void immediate_basis(Harness &harness)
{
	for (unsigned bit = 0; bit < 19; ++bit) {
		const auto displacement = bit == 18U ?
			-(std::int64_t{1} << 20) :
			std::int64_t{4} << bit;
		const auto instruction =
			encode_compare_branch(true, false, displacement, 7);
		auto initial = seeded_state(
			0x1410'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(instruction) << 5));
		initial.registers[7] = 0;
		const auto target = static_cast<std::uint64_t>(
			static_cast<std::int64_t>(branch_pc) + displacement);
		char name[128];
		std::snprintf(name, sizeof(name),
			"CBZ immediate encoding bit=%u", bit);
		harness.check_branch(
			name, instruction, initial, initial, target);
	}
	for (unsigned bit = 0; bit < 14; ++bit) {
		const auto displacement = bit == 13U ?
			-(std::int64_t{1} << 15) :
			std::int64_t{4} << bit;
		const auto instruction =
			encode_test_branch(false, 37, displacement, 11);
		auto initial = seeded_state(
			0x1510'0000'0000'0000ULL ^
				(static_cast<std::uint64_t>(instruction) << 5));
		initial.registers[11] &= ~(std::uint64_t{1} << 37);
		const auto target = static_cast<std::uint64_t>(
			static_cast<std::int64_t>(branch_pc) + displacement);
		char name[128];
		std::snprintf(name, sizeof(name),
			"TBZ immediate encoding bit=%u", bit);
		harness.check_branch(
			name, instruction, initial, initial, target);
	}
}

/*
 * A taken CBZ or CBNZ closing a short loop, "cb{n}z {w,x}0, .-8": imm19 is -2,
 * whose low five bits are 30. The loop body counts iterations in x1 and then
 * steps x0; the counter passes a value whose low word and full register
 * disagree about zero, so the operand width decides when the loop exits. A
 * target one word off skips the count or never ends the loop.
 */
void counted_loops(Harness &harness)
{
	constexpr std::uint32_t count_iteration = 0x9100'0421U; // add x1, x1, #1
	constexpr std::uint32_t subtract_one = 0xd100'0400U;    // sub x0, x0, #1
	constexpr std::uint32_t subtract_x2 = 0xcb02'0000U;     // sub x0, x0, x2
	constexpr std::uint32_t add_x2 = 0x8b02'0000U;          // add x0, x0, x2
	struct Loop final {
		bool wide;
		bool nonzero;
		std::uint32_t step;
		std::uint64_t counter;
		std::uint64_t operand;
		unsigned iterations;
	};
	constexpr std::array loops{
		/* x0 counts down to 0x1'0000'0000, whose low word is zero. */
		Loop{false, true, subtract_one, 0x1'0000'0003ULL, 0, 3},
		/* x0 passes 0x1'0000'0000 on its way down to zero. */
		Loop{true, true, subtract_x2, 0x1'8000'0000ULL, 0x8000'0000ULL, 3},
		/* 0x2'0000'0000 has a zero low word; 0x3'0000'0001 does not. */
		Loop{false, false, add_x2, 0xffff'ffffULL, 0x1'0000'0001ULL, 2},
		/* x0 wraps to zero, then reaches 0x1'0000'0000. */
		Loop{true, false, add_x2, 0xffff'ffff'0000'0000ULL,
			0x1'0000'0000ULL, 2},
	};
	for (const auto &loop : loops) {
		const auto branch = encode_compare_branch(
			loop.wide, loop.nonzero, -8, 0);
		const std::array code{
			ProgramWord{.offset = branch_entry_offset,
				.instruction = count_iteration},
			ProgramWord{.offset = branch_entry_offset + 4,
				.instruction = loop.step},
			ProgramWord{.offset = branch_entry_offset + 8,
				.instruction = branch},
			ProgramWord{.offset = branch_entry_offset + 12,
				.instruction = terminal_system_call},
		};
		auto initial = seeded_state(0x1420'0000'0000'0000ULL ^
			(static_cast<std::uint64_t>(branch) << 7));
		initial.registers[0] = loop.counter;
		if (loop.step != subtract_one)
			initial.registers[2] = loop.operand;
		/* The architectural result: each iteration steps x0 with
		 * 64-bit wrap-around, then the branch tests the low word or the
		 * whole register. */
		const auto mask = loop.wide ? ~std::uint64_t{0} :
			std::uint64_t{0xffff'ffffU};
		auto counter = loop.counter;
		unsigned iterations = 0;
		for (bool taken = true; taken;) {
			require(++iterations <= loop.iterations,
				"counted loop runs longer than intended");
			counter = loop.step == subtract_one ? counter - 1U :
				loop.step == subtract_x2 ? counter - loop.operand :
				counter + loop.operand;
			taken = loop.nonzero ? (counter & mask) != 0 :
				(counter & mask) == 0;
		}
		require(iterations == loop.iterations,
			"counted loop exits earlier than intended");
		auto expected = initial;
		expected.registers[0] = counter;
		expected.registers[1] = initial.registers[1] + loop.iterations;
		expected.pc = branch_pc + 16;
		char name[96];
		std::snprintf(name, sizeof(name), "%s %c0 counted loop .-8",
			loop.nonzero ? "CBNZ" : "CBZ", loop.wide ? 'x' : 'w');
		harness.check_program({
			.name = name,
			.code = std::span<const ProgramWord>{code},
			.entry_offset = branch_entry_offset,
			.initial = initial,
			.expected = expected,
			.native = NativeRequirements::baseline(),
			.expected_instructions = 3U * loop.iterations + 1U,
		});
	}
}

void production_specialization()
{
	constexpr auto base = engine_test_code_base;
	constexpr std::int64_t displacement = 0x100;
	constexpr auto target = base + displacement;
	std::set<std::uintptr_t> compare_entries;
	for (const bool wide : {false, true})
		for (const bool nonzero : {false, true}) {
			const auto built = gadgets::Builder{}.build(
				base, std::array{encode_compare_branch(
					wide, nonzero, displacement, 7)});
			require(built.complete &&
					built.program.words().size() == 4U,
				"compare branch retained unexpected runtime fields");
			require(built.program.words()[1] ==
					offsetof(arm64::CpuState, registers) +
						7U * 8U &&
					built.program.words()[2] == target &&
					built.program.words()[3] == base + 4,
				"compare branch fields were not resolved during "
				"translation");
			compare_entries.insert(built.program.words()[0]);
		}
	require(compare_entries.size() == 4U,
		"compare branch width/condition did not select four gadgets");

	std::set<std::uintptr_t> test_entries;
	for (const bool nonzero : {false, true})
		for (const unsigned bit : {0U, 1U, 31U, 32U, 63U}) {
			const auto built = gadgets::Builder{}.build(
				base, std::array{encode_test_branch(
					nonzero, bit, displacement, 11)});
			require(built.complete &&
					built.program.words().size() == 5U,
				"test branch retained unexpected runtime fields");
			require(built.program.words()[1] ==
					offsetof(arm64::CpuState, registers) +
						11U * 8U &&
					built.program.words()[2] ==
						(std::uint64_t{1} << bit) &&
					built.program.words()[3] == target &&
					built.program.words()[4] == base + 4,
				"test branch fields were not resolved during "
				"translation");
			test_entries.insert(built.program.words()[0]);
		}
	require(test_entries.size() == 2U,
		"test branch condition did not select two gadgets");

	for (const bool wide : {false, true})
		for (const bool nonzero : {false, true}) {
			const auto zero = gadgets::Builder{}.build(
				base, std::array{encode_compare_branch(
					wide, nonzero, displacement, 31)});
			require(zero.complete &&
					zero.program.words().size() == 2U &&
					zero.program.words()[1] ==
						(nonzero ? base + 4 : target),
				"XZR compare branch was not folded to a fixed edge");
		}
	for (const bool nonzero : {false, true}) {
		const auto zero = gadgets::Builder{}.build(
			base, std::array{encode_test_branch(
				nonzero, 63, displacement, 31)});
		require(zero.complete && zero.program.words().size() == 2U &&
				zero.program.words()[1] ==
					(nonzero ? base + 4 : target),
			"XZR test branch was not folded to a fixed edge");
	}
}

} // namespace
} // namespace uml::emulator::tests::base_control

int main()
{
	uml::emulator::tests::base_control::Harness harness;
	uml::emulator::tests::base_control::compare_semantics(harness);
	uml::emulator::tests::base_control::test_semantics(harness);
	uml::emulator::tests::base_control::immediate_basis(harness);
	uml::emulator::tests::base_control::counted_loops(harness);
	uml::emulator::tests::base_control::production_specialization();
	harness.print_summary("arm64-base-control-compare-test");
	return 0;
}
