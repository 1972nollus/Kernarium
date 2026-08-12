// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_trap_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdio>

namespace test = arm64_floating_trap_test;

namespace {

void scalar_precision_variants(test::engine::EngineHarness &harness)
{
	struct Form final {
		const char *name;
		std::uint32_t instruction;
		unsigned lane_bytes;
		std::uint64_t one;
		test::engine::NativeRequirements native;
	};
	const std::array forms{
		Form{"half", 0x1ee2'1820U, 2U, 0x3c00U,
			test::scalar_half_native()},
		Form{"single", 0x1e22'1820U, 4U, 0x3f80'0000U,
			test::scalar_native()},
		Form{"double", 0x1e62'1820U, 8U,
			0x3ff0'0000'0000'0000ULL, test::scalar_native()},
	};
	std::uint64_t seed = 1;
	for (const auto &form : forms) {
		auto initial = test::state(seed++);
		initial.fpcr = test::enable_for(test::fpsr_divide_by_zero);
		initial.fpsr |= test::fpsr_input_denormal;
		test::write_lane(initial, 1U, 0U, form.lane_bytes, form.one);
		test::write_lane(initial, 2U, 0U, form.lane_bytes, 0U);
		char name[96];
		std::snprintf(name, sizeof(name),
			"scalar enabled divide-by-zero precision=%s", form.name);
		test::check_trap(harness, name, form.instruction, initial,
			test::fpsr_divide_by_zero, 0U, form.native);
	}
}

void every_exception_cause(test::engine::EngineHarness &harness)
{
	{
		auto initial = test::state(20U);
		initial.fpcr = test::enable_for(test::fpsr_invalid);
		test::write_lane(initial, 4U, 0U, 4U, 0xbf80'0000U);
		test::check_trap(harness, "scalar enabled invalid operation",
			0x1e21'c083U, initial, test::fpsr_invalid, 0U,
			test::scalar_native());
	}
	{
		auto initial = test::state(21U);
		initial.fpcr = test::enable_for(test::fpsr_overflow);
		test::write_lane(initial, 6U, 0U, 4U, 0x7f7f'ffffU);
		test::write_lane(initial, 7U, 0U, 4U, 0x4000'0000U);
		test::check_trap(harness, "scalar enabled overflow",
			0x1e27'08c5U, initial, test::fpsr_overflow,
			test::fpsr_inexact, test::scalar_native());
	}
	{
		auto initial = test::state(22U);
		initial.fpcr = test::enable_for(test::fpsr_underflow);
		test::write_lane(initial, 6U, 0U, 4U, 0x0000'0001U);
		test::write_lane(initial, 7U, 0U, 4U, 0x3f00'0000U);
		test::check_trap(harness, "scalar enabled underflow",
			0x1e27'08c5U, initial, test::fpsr_underflow,
			test::fpsr_inexact, test::scalar_native());
	}
	{
		auto initial = test::state(23U);
		initial.fpcr = test::enable_for(test::fpsr_inexact);
		test::write_lane(initial, 1U, 0U, 4U, 0x3f80'0000U);
		test::write_lane(initial, 2U, 0U, 4U, 0x4040'0000U);
		test::check_trap(harness, "scalar enabled inexact",
			0x1e22'1820U, initial, test::fpsr_inexact, 0U,
			test::scalar_native());
	}
	{
		auto initial = test::state(24U);
		initial.fpcr = test::fpcr_flush_to_zero |
			test::enable_for(test::fpsr_input_denormal);
		test::write_lane(initial, 9U, 0U, 4U, 0x0000'0001U);
		test::write_lane(initial, 10U, 0U, 4U, 0x3f80'0000U);
		test::check_trap(harness, "scalar enabled input denormal",
			0x1e2a'2928U, initial, test::fpsr_input_denormal, 0U,
			test::scalar_native());
	}
}

void simultaneous_causes(test::engine::EngineHarness &harness)
{
	auto initial = test::state(40U);
	initial.fpcr = test::enable_for(
		test::fpsr_overflow | test::fpsr_inexact);
	initial.fpsr |= test::fpsr_invalid;
	test::write_lane(initial, 6U, 0U, 4U, 0x7f7f'ffffU);
	test::write_lane(initial, 7U, 0U, 4U, 0x4000'0000U);
	test::check_trap(harness, "scalar simultaneous enabled causes",
		0x1e27'08c5U, initial,
		test::fpsr_overflow | test::fpsr_inexact, 0U,
		test::scalar_native());
}

void comparison_rolls_back_nzcv(test::engine::EngineHarness &harness)
{
	/* FCMPE S6, S7 would report unordered in NZCV for the quiet NaN.
	 * With invalid-operation enabled, the comparison instead traps before
	 * committing those flags.
	 */
	constexpr std::uint32_t instruction = 0x1e27'20d0U;
	auto initial = test::state(41U);
	initial.fpcr = test::enable_for(test::fpsr_invalid);
	initial.fpsr |= test::fpsr_underflow;
	initial.nzcv = 0xa000'0000U;
	test::write_lane(initial, 6U, 0U, 4U, 0x7fc1'2345U);
	test::write_lane(initial, 7U, 0U, 4U, 0x3f80'0000U);
	test::check_trap(harness, "comparison NZCV rollback",
		instruction, initial, test::fpsr_invalid, 0U,
		test::scalar_native());
}

void javascript_conversion_rolls_back_x8_x9_and_nzcv(
	test::engine::EngineHarness &harness)
{
	/* FJCVTZS W8/W9, D9 would write zero and clear NZCV for +Infinity.
	 * The enabled invalid-operation trap must preserve each complete fixed-bank
	 * destination, the other bank register, and the pre-instruction flags.
	 */
	constexpr std::array instructions{
		0x1e7e'0128U, // fjcvtzs w8, d9
		0x1e7e'0129U, // fjcvtzs w9, d9
	};
	constexpr std::array names{
		"JavaScript conversion X8 and NZCV rollback",
		"JavaScript conversion X9 and NZCV rollback",
	};
	for (std::size_t index = 0; index < instructions.size(); ++index) {
		auto initial = test::state(42U + index);
		initial.fpcr = test::enable_for(test::fpsr_invalid);
		initial.fpsr |= test::fpsr_divide_by_zero;
		initial.registers[8] = 0xfedc'ba98'7654'3210ULL;
		initial.registers[9] = 0x0123'4567'89ab'cdefULL;
		initial.nzcv = 0xb000'0000U;
		test::write_lane(initial, 9U, 0U, 8U,
			0x7ff0'0000'0000'0000ULL);
		test::check_trap(harness, names[index], instructions[index], initial,
			test::fpsr_invalid, 0U,
			test::engine::NativeRequirements::features(
				::uml::cpu::ArmFeature::jscvt));
	}
}

} // namespace

int main()
{
	test::engine::EngineHarness harness;
	scalar_precision_variants(harness);
	every_exception_cause(harness);
	simultaneous_causes(harness);
	comparison_rolls_back_nzcv(harness);
	javascript_conversion_rolls_back_x8_x9_and_nzcv(harness);
	harness.print_summary("arm64-floating-trap-scalar");
	return 0;
}
