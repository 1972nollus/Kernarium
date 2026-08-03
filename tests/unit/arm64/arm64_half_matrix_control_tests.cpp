// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_matrix_test_support.hpp"

namespace test = arm64_half_matrix_test;

namespace {

[[nodiscard]] test::arm64::CpuState empty_state(unsigned seed,
	std::uint32_t fpcr)
{
	auto cpu = test::state(seed);
	test::clear_vectors(cpu);
	cpu.fpcr = fpcr;
	cpu.fpsr = 0x0800'0000U;
	return cpu;
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts{};

	auto rounding = empty_state(500U, 0U);
	test::write_half(rounding, 1U, 0U, 0x3c01U);
	test::write_half(rounding, 2U, 0U, 0x3c01U);
	test::check(harness, "half-matrix-nearest-rounding",
		false, {0U, 1U, 2U}, rounding, counts);
	rounding.fpcr = 1U << 22U;
	test::check(harness, "half-matrix-upward-rounding",
		false, {0U, 1U, 2U}, rounding, counts);

	auto nan = empty_state(501U, test::shared::fpcr_ah);
	test::write_half(nan, 1U, 0U, 0x7e01U);
	test::write_half(nan, 2U, 0U, 0x7c02U);
	test::check(harness, "half-matrix-alternative-nan",
		false, {0U, 1U, 2U}, nan, counts);

	auto flush_half = empty_state(
		502U, test::shared::fpcr_fz16);
	test::write_half(flush_half, 1U, 0U, 0x0400U);
	test::write_half(flush_half, 2U, 0U, 0x3800U);
	test::check(harness, "half-matrix-standard-half-flush",
		false, {0U, 1U, 2U}, flush_half, counts);
	flush_half.fpcr |= test::shared::fpcr_ah;
	test::check(harness, "half-matrix-alternative-half-flush",
		false, {0U, 1U, 2U}, flush_half, counts);

	auto invalid = empty_state(
		503U, test::shared::fpcr_ah |
			test::shared::fpcr_fz16);
	test::write_half(invalid, 1U, 0U, 1U);
	test::write_half(invalid, 2U, 0U, 0x7c00U);
	test::check(harness, "half-matrix-invalid-product",
		false, {0U, 1U, 2U}, invalid, counts);

	auto fiz = empty_state(504U, test::shared::fpcr_fiz);
	test::write_single(fiz, 0U, 0U, 1U);
	test::check(harness, "half-matrix-wide-input-zero",
		true, {0U, 1U, 2U}, fiz, counts);
	auto ah_fz = empty_state(505U, test::shared::fpcr_ah |
		test::shared::fpcr_fz);
	test::write_single(ah_fz, 0U, 0U, 1U);
	test::check(harness, "half-matrix-wide-output-flush",
		true, {0U, 1U, 2U}, ah_fz, counts);
	auto dot_nan = empty_state(506U, test::shared::fpcr_ah |
		test::shared::fpcr_dn);
	test::write_half(dot_nan, 1U, 0U, 0x7c01U);
	test::write_half(dot_nan, 2U, 0U, test::half(1.0F));
	test::check(harness, "half-matrix-wide-default-nan",
		true, {0U, 1U, 2U}, dot_nan, counts);

	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-matrix-control");
}
