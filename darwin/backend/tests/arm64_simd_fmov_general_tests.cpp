// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fmov_test_support.hpp"

namespace test = arm64_simd_fmov_test;
namespace base = arm64_simd_test;

namespace {

void fp_to_gpr_low_high_and_discard()
{
	base::Harness harness;
	struct Case final { std::uint32_t opcode; unsigned offset; unsigned bytes; };
	for (const auto test_case : {Case{0x1ee6'0000U, 0U, 2U},
		     Case{0x9ee6'0000U, 0U, 2U}, Case{0x1e26'0000U, 0U, 4U},
		     Case{0x9e66'0000U, 0U, 8U}, Case{0x9eae'0000U, 8U, 8U}}) {
		auto initial = base::state(test_case.opcode);
		initial.fpsr = 0x0800'0090U;
		const auto encoded = test_case.opcode | (29U << 5) | 28U;
		auto expected = initial;
		expected.pc = base::pc + 4U;
		expected.registers[28] = test::read_bits(initial.vectors[29].bytes,
			test_case.offset, test_case.bytes);
		base::require_architecture_equal(expected,
			harness.run_interpreter(encoded, initial));
		harness.require_differential(encoded, initial);

		const auto discard = test_case.opcode | (29U << 5) | 31U;
		expected = initial;
		expected.pc = base::pc + 4U;
		base::require_architecture_equal(expected,
			harness.run_interpreter(discard, initial));
		harness.require_differential(discard, initial);
	}
}

void gpr_to_fp_low_high_and_zero_source()
{
	base::Harness harness;
	struct Case final { std::uint32_t opcode; unsigned offset; unsigned bytes; };
	for (const auto test_case : {Case{0x1ee7'0000U, 0U, 2U},
		     Case{0x9ee7'0000U, 0U, 2U}, Case{0x1e27'0000U, 0U, 4U},
		     Case{0x9e67'0000U, 0U, 8U}, Case{0x9eaf'0000U, 8U, 8U}}) {
		for (const unsigned source : {29U, 31U}) {
			auto initial = base::state(test_case.opcode + source);
			initial.fpsr = 0x0800'0090U;
			initial.registers[29] = 0xfedc'ba98'7654'3210ULL;
			const auto encoded = test_case.opcode | (source << 5) | 28U;
			auto expected = initial;
			expected.pc = base::pc + 4U;
			const auto value = source == 31U ? 0U : initial.registers[29];
			if (test_case.offset == 0U)
				expected.vectors[28].bytes = {};
			test::write_bits(expected.vectors[28].bytes, test_case.offset,
				test_case.bytes, value);
			base::require_architecture_equal(expected,
				harness.run_interpreter(encoded, initial));
			harness.require_differential(encoded, initial);
		}
	}
}

} // namespace

int main()
{
	fp_to_gpr_low_high_and_discard();
	gpr_to_fp_low_high_and_zero_source();
	return 0;
}
