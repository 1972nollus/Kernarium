// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_basic_floating_test_support.hpp"
#include "cpu/arm64/detail/floating_round.hpp"

namespace test = arm64_basic_floating_test;

namespace {

void every_scalar_and_vector_allocation()
{
	test::common::Harness harness;
	unsigned forms{};
	test::for_each_form([&](const test::Form &form) {
		++forms;
		harness.require_differential(test::instruction(form,
			{31, 30, 29}), test::common::state(forms));
	});
	test::common::require(forms == 43U);
}

void reserved_precisions_fail_before_state_changes()
{
	test::common::Harness harness;
	for (const auto operation : test::scalar_operations) {
		auto instruction = test::instruction(
			{operation, true, 32U, 16U}, {0, 1, 2});
		instruction &= ~0x00c0'0000U;
		instruction |= 2U << 22;
		harness.require_illegal(instruction, test::common::state(77));
	}
	for (const auto operation : test::vector_operations) {
		const test::Form form{operation, false, 64U, 16U};
		auto instruction = test::instruction(form, {0, 1, 2});
		instruction &= ~0x4000'0000U;
		harness.require_illegal(instruction, test::common::state(88));
	}
}

void vector_fsqrt_does_not_claim_the_frint64x_allocation()
{
	using test::detail::decode_basic_floating_arithmetic;
	using test::detail::decode_floating_round;

	constexpr auto fsqrt_2s =
		test::instruction({test::Operation::square_root, false, 32U, 8U},
			{0U, 0U, 0U});
	constexpr auto fsqrt_4s =
		test::instruction({test::Operation::square_root, false, 32U, 16U},
			{0U, 0U, 0U});
	constexpr auto fsqrt_2d =
		test::instruction({test::Operation::square_root, false, 64U, 16U},
			{0U, 0U, 0U});
	test::common::require(fsqrt_2s == 0x2ea1'f800U);
	test::common::require(fsqrt_4s == 0x6ea1'f800U);
	test::common::require(fsqrt_2d == 0x6ee1'f800U);
	test::common::require(
		decode_basic_floating_arithmetic(fsqrt_2s).recognized);

	/* With bit 23 clear this is the valid FRINT64X Vd.2S,Vn.2S
	 * allocation, not a malformed FSQRT.  Keep the family decoders
	 * disjoint even if their dispatcher ordering changes.
	 */
	constexpr std::uint32_t frint64x_2s = 0x2e21'f800U;
	test::common::require(
		!decode_basic_floating_arithmetic(frint64x_2s).recognized);
	const auto rounded = decode_floating_round(frint64x_2s);
	test::common::require(rounded.recognized);
	test::common::require(rounded.valid);
}

} // namespace

int main()
{
	every_scalar_and_vector_allocation();
	reserved_precisions_fail_before_state_changes();
	vector_fsqrt_does_not_claim_the_frint64x_allocation();
}
