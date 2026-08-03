// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_basic_floating_test_support.hpp"
#include "cpu/arm64/detail/floating_round.hpp"

namespace test = arm64_basic_floating_test;

namespace {

void check_valid(test::engine_tests::EngineHarness &harness,
	const test::Form &form, test::Registers registers, unsigned seed)
{
	auto initial = test::state(seed);
	initial.fpsr = (seed & 1U) != 0U ? 1U << 27 : 0U;
	test::prepare_exact(initial, form, registers);
	harness.check_one("basic-floating-valid-allocation",
		test::instruction(form, registers), initial,
		test::expected_exact(initial, form, registers),
		test::native_requirements(form));
}

void every_valid_register_field_and_alias()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed{};
	unsigned forms{};
	test::for_each_form([&](const test::Form &form) {
		++forms;
		check_valid(harness, form, {0, 1, 2}, ++seed);
		for (unsigned reg = 0; reg < 32; ++reg) {
			check_valid(harness, form, {reg, 1, 2}, ++seed);
			check_valid(harness, form, {0, reg, 2}, ++seed);
			if (form.operation != test::Operation::square_root)
				check_valid(harness, form, {0, 1, reg}, ++seed);
		}
	});
	test::common::require(forms == 43U);
	harness.print_summary("arm64-basic-floating-allocation-valid");
}

void every_reserved_precision_and_arrangement()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 7000;
	const auto check_fields = [&](const test::Form &form,
				      std::uint32_t encoded) {
		const auto check = [&](test::Registers registers) {
			const auto register_bits =
				(registers.destination & 31U) |
				((registers.left & 31U) << 5) |
				(form.operation == test::Operation::square_root ? 0U :
					(registers.right & 31U) << 16);
			const std::uint32_t register_mask =
				form.operation == test::Operation::square_root ?
					0x0000'03ffU : 0x001f'03ffU;
			const auto instruction =
				(encoded & ~register_mask) |
				register_bits;
			harness.check_illegal("basic-floating-reserved",
				instruction, test::state(++seed));
		};
		check({0, 1, 2});
		for (unsigned reg = 0; reg < 32; ++reg) {
			check({reg, 1, 2});
			check({0, reg, 2});
			if (form.operation != test::Operation::square_root)
				check({0, 1, reg});
		}
	};

	for (const auto operation : test::scalar_operations) {
		const test::Form form{operation, true, 32U, 16U};
		auto encoded = test::instruction(form, {0, 1, 2});
		encoded &= ~0x00c0'0000U;
		encoded |= 2U << 22;
		check_fields(form, encoded);
	}
	for (const auto operation : test::vector_operations) {
		const test::Form form{operation, false, 64U, 16U};
		const auto encoded =
			test::instruction(form, {0, 1, 2}) & ~0x4000'0000U;
		check_fields(form, encoded);
	}
	harness.print_summary("arm64-basic-floating-allocation-reserved");
}

void neighboring_allocation_remains_disjoint()
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

	constexpr std::uint32_t neighboring_2s = 0x2e21'f800U;
	test::common::require(
		!decode_basic_floating_arithmetic(neighboring_2s).recognized);
	const auto rounded = decode_floating_round(neighboring_2s);
	test::common::require(rounded.recognized);
	test::common::require(rounded.valid);
}

} // namespace

int main()
{
	every_valid_register_field_and_alias();
	every_reserved_precision_and_arrangement();
	neighboring_allocation_remains_disjoint();
	return 0;
}
