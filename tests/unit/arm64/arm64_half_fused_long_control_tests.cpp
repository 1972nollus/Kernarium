// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_half_fused_long_test_support.hpp"

namespace test = arm64_half_fused_long_test;

namespace {

[[nodiscard]] test::arm64::CpuState one_value(std::uint32_t addend,
	std::uint16_t left, std::uint16_t right, std::uint32_t fpcr)
{
	auto initial = test::state(500U);
	initial.fpcr = fpcr;
	initial.fpsr = 0x0800'0000U;
	for (unsigned lane = 0U; lane < 4U; ++lane) {
		test::write_single(initial, 0U, lane, addend);
		test::write_half(initial, 1U, lane, left);
		test::write_half(initial, 2U, lane, right);
	}
	return initial;
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	test::ExpectedCounts counts{};
	const test::Form add{false, false, false, true, 0U};
	const test::Form subtract{false, true, false, true, 0U};

	struct ControlCase final {
		std::uint32_t addend;
		std::uint16_t left;
		std::uint16_t right;
		std::uint32_t fpcr;
	};
	constexpr std::array cases{
		ControlCase{0x3f80'0000U, 0x0c00U, 0x0c00U, 0U},
		ControlCase{0x3f80'0000U, 0x0c00U, 0x0c00U, 1U << 22U},
		ControlCase{0U, 1U, 0x3c00U, test::shared::fpcr_fz16},
		ControlCase{0U, 0U, 0x7c00U, 0U},
		ControlCase{0x7fc1'0000U, 0xfc01U, 0x7e03U,
			test::shared::fpcr_ah},
		ControlCase{1U, 0U, 0x3c00U, test::shared::fpcr_fiz},
		ControlCase{1U, 0U, 0x3c00U, test::shared::fpcr_ah},
		ControlCase{0x7f7f'ffffU, 0x7bffU, 0x7bffU, 0U},
		ControlCase{0x7f7f'ffffU, 0x7bffU, 0x7bffU, 3U << 22U},
		ControlCase{0x0080'0000U, 0x8001U, 0x3c00U,
			test::shared::fpcr_ah | test::shared::fpcr_fz},
		ControlCase{0x7fc1'2345U, 0x7c01U, 0x7e02U,
			test::shared::fpcr_dn},
	};
	unsigned index{};
	for (const auto &control : cases) {
		auto initial = one_value(control.addend, control.left,
			control.right, control.fpcr);
		test::check(harness, "half-fused-long-control-add", add,
			{0U, 1U, 2U}, initial, counts);
		if ((index++ & 1U) != 0U)
			test::check(harness,
				"half-fused-long-control-subtract", subtract,
				{0U, 1U, 2U}, initial, counts);
	}

	test::require_counts(harness, counts);
	harness.print_summary("arm64-half-fused-long-control");
}
