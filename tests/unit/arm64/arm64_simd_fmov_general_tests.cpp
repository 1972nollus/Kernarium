// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fmov_test_support.hpp"

namespace test = arm64_simd_fmov_test;
namespace base = arm64_simd_test;

namespace {

struct Case final {
	std::uint32_t opcode;
	unsigned offset;
	unsigned bytes;
};

[[nodiscard]] test::engine_tests::NativeRequirements requirements(
	const Case &test_case)
{
	return test::native_requirements(test_case.bytes);
}

void fp_to_gpr_low_high_and_discard()
{
	test::engine_tests::EngineHarness harness;
	for (const auto test_case : {Case{0x1ee6'0000U, 0U, 2U},
		     Case{0x9ee6'0000U, 0U, 2U}, Case{0x1e26'0000U, 0U, 4U},
		     Case{0x9e66'0000U, 0U, 8U}, Case{0x9eae'0000U, 8U, 8U}})
		for (unsigned destination = 0; destination < 32U; ++destination)
			for (unsigned source = 0; source < 32U; ++source) {
				auto initial = base::state(test_case.opcode +
					destination * 3U + source * 7U);
				initial.fpsr = 0x0800'0090U;
				auto expected = initial;
				if (destination != 31U)
					expected.registers[destination] =
						test::read_bits(
							initial.vectors[source].bytes,
							test_case.offset,
							test_case.bytes);
				harness.check_one("fmov-vector-to-general",
					test_case.opcode | (source << 5) | destination,
					initial, expected, requirements(test_case));
			}
	harness.print_summary("arm64-simd-fmov-general-to-gpr");
}

void gpr_to_fp_low_high_and_zero_source()
{
	test::engine_tests::EngineHarness harness;
	for (const auto test_case : {Case{0x1ee7'0000U, 0U, 2U},
		     Case{0x9ee7'0000U, 0U, 2U}, Case{0x1e27'0000U, 0U, 4U},
		     Case{0x9e67'0000U, 0U, 8U}, Case{0x9eaf'0000U, 8U, 8U}})
		for (unsigned destination = 0; destination < 32U; ++destination)
			for (unsigned source = 0; source < 32U; ++source) {
				auto initial = base::state(test_case.opcode +
					destination * 5U + source * 11U);
				initial.fpsr = 0x0800'0090U;
				initial.registers[source] =
					0xfedc'ba98'7654'3210ULL ^
					(static_cast<std::uint64_t>(source) << 37);
				auto expected = initial;
				const auto value =
					source == 31U ? 0U : initial.registers[source];
				if (test_case.offset == 0U)
					expected.vectors[destination].bytes = {};
				test::write_bits(expected.vectors[destination].bytes,
					test_case.offset, test_case.bytes, value);
				harness.check_one("fmov-general-to-vector",
					test_case.opcode | (source << 5) | destination,
					initial, expected, requirements(test_case));
			}
	harness.print_summary("arm64-simd-fmov-general-to-fp");
}

} // namespace

int main()
{
	fp_to_gpr_low_high_and_discard();
	gpr_to_fp_low_high_and_zero_source();
	return 0;
}
