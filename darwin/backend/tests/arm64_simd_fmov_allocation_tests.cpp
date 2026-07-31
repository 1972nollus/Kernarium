// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fmov_test_support.hpp"

namespace test = arm64_simd_fmov_test;
namespace base = arm64_simd_test;

namespace {

void every_scalar_precision_and_immediate()
{
	test::engine_tests::EngineHarness harness;
	for (const unsigned bytes : {2U, 4U, 8U}) {
		for (unsigned destination = 0; destination < 32U; ++destination)
			for (unsigned source = 0; source < 32U; ++source) {
				auto initial = base::state(bytes + destination * 3U +
					source * 7U);
				initial.fpsr = 0x0800'0090U;
				test::write_bits(initial.vectors[source].bytes, 0U, bytes,
					0xfedc'ba98'7654'3210ULL);
				test::check_scalar_result(harness,
					"fmov-register-valid-allocation",
					test::register_instruction(bytes, destination, source),
					bytes, destination,
					test::read_bits(initial.vectors[source].bytes, 0U,
						bytes),
					initial);
			}
		for (unsigned destination = 0; destination < 32U; ++destination)
			for (unsigned immediate = 0; immediate < 256U; ++immediate) {
				auto initial = base::state(bytes + destination * 5U +
					immediate * 11U);
				initial.fpsr = 0x0800'0090U;
				test::check_scalar_result(harness,
					"fmov-immediate-valid-allocation",
					test::immediate_instruction(bytes,
						static_cast<std::uint8_t>(immediate),
						destination),
					bytes, destination,
					test::expand_immediate(
						static_cast<std::uint8_t>(immediate),
						bytes * 8U),
					initial);
			}
	}
	harness.print_summary("arm64-simd-fmov-allocation-valid");
}

void reserved_scalar_ftype_is_early_illegal()
{
	test::engine_tests::EngineHarness harness;
	for (unsigned destination = 0; destination < 32U; ++destination) {
		for (unsigned source = 0; source < 32U; ++source) {
			const auto bad_register = 0x1e20'4000U | (2U << 22) |
				(source << 5) | destination;
			harness.check_illegal("fmov-register-reserved-type",
				bad_register,
				base::state(31U + destination + source));
		}
		for (unsigned immediate = 0; immediate < 256U; ++immediate) {
			const auto bad_immediate = 0x1e20'1000U | (2U << 22) |
				(immediate << 13) | destination;
			harness.check_illegal("fmov-immediate-reserved-type",
				bad_immediate,
				base::state(32U + destination + immediate));
		}
	}
	harness.print_summary("arm64-simd-fmov-allocation-reserved");
}

} // namespace

int main()
{
	every_scalar_precision_and_immediate();
	reserved_scalar_ftype_is_early_illegal();
	return 0;
}
