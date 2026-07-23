// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_fused_floating_test_support.hpp"

#include <limits>

namespace test = arm64_fused_floating_test;

namespace {

template<typename Floating>
void scalar_nep_merges_from_addend(unsigned width)
{
	test::common::Harness harness;
	constexpr test::Registers registers{0, 1, 2, 3};
	for (const bool negate : {false, true})
		for (const bool subtract : {false, true}) {
			const test::Form form{true, negate, subtract, width, 16U};
			auto initial = test::common::state(
				width + negate * 7U + subtract * 13U);
			initial.fpcr = 4U;
			initial.fpsr = 0;
			test::prepare_exact<Floating>(initial, form, registers);
			const auto addend = initial.vectors[registers.addend].bytes;
			const auto actual = harness.run_interpreter(
				test::instruction(form, registers), initial);
			for (unsigned byte = sizeof(Floating); byte < 16U; ++byte)
				test::common::require(
					actual.vectors[registers.destination].bytes[byte] ==
					addend[byte]);
			harness.require_differential(
				test::instruction(form, registers), initial);
		}
}

void invalid_fused_operation_sets_ioc_and_preserves_prior_fpsr()
{
	test::common::Harness harness;
	constexpr test::Registers registers{0, 1, 2, 3};
	const test::Form form{true, false, false, 64U, 16U};
	auto initial = test::common::state(0);
	initial.fpsr = 1U << 4;
	test::basic::set_lane(initial, 1, 0,
		std::numeric_limits<double>::infinity());
	test::basic::set_lane(initial, 2, 0, 0.0);
	test::basic::set_lane(initial, 3, 0, 1.0);
	const auto actual = harness.run_interpreter(
		test::instruction(form, registers), initial);
	test::common::require((actual.fpsr & 1U) != 0U);
	test::common::require((actual.fpsr & (1U << 4)) != 0U);
	harness.require_differential(test::instruction(form, registers), initial);
}

} // namespace

int main()
{
	scalar_nep_merges_from_addend<_Float16>(16U);
	scalar_nep_merges_from_addend<float>(32U);
	scalar_nep_merges_from_addend<double>(64U);
	invalid_fused_operation_sets_ioc_and_preserves_prior_fpsr();
}
