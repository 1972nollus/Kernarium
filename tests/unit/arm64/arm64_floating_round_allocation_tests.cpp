// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_floating_round_test_support.hpp"

namespace test = arm64_floating_round_test;

namespace {

void check_valid(test::engine_tests::EngineHarness &harness,
	const test::Form &form, test::Registers registers, unsigned seed)
{
	auto initial = test::state(seed);
	const auto input = test::dyadic_bits(
		test::format(form.width), 3U, 0, (seed & 1U) != 0U);
	test::fill_active_lanes(initial, registers.source, form, input);
	harness.check_one(test::inventory_name(form),
		test::instruction(form, registers), initial,
		test::expected_state(initial, form, registers),
		test::native_requirements(form));
}

void every_valid_register_encoding(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed{};
	unsigned forms{};
	test::for_each_form([&](const test::Form &form) {
		++forms;
		for (unsigned destination = 0; destination < 32; ++destination)
			for (unsigned source = 0; source < 32; ++source)
				check_valid(harness, form,
					{destination, source}, ++seed);
	});
	if (forms != 76U)
		__builtin_trap();
}

void check_reserved_registers(test::engine_tests::EngineHarness &harness,
	std::uint32_t instruction, unsigned &seed)
{
	for (unsigned destination = 0; destination < 32; ++destination)
		for (unsigned source = 0; source < 32; ++source) {
			const auto encoded = (instruction & ~0x3ffU) |
				(source << 5) | destination;
			harness.check_illegal("floating-round-reserved-allocation",
				encoded, test::state(++seed));
		}
}

void every_reserved_precision_and_arrangement(
	test::engine_tests::EngineHarness &harness)
{
	unsigned seed = 100'000U;
	for (const auto operation : test::standard_operations) {
		auto encoded = test::instruction(
			{operation, true, 32U, 16U});
		encoded &= ~0x00c0'0000U;
		encoded |= 2U << 22;
		check_reserved_registers(harness, encoded, seed);
	}
	for (const auto operation : test::sized_operations)
		for (const unsigned type : {2U, 3U}) {
			auto encoded = test::instruction(
				{operation, true, 32U, 16U});
			encoded &= ~0x00c0'0000U;
			encoded |= type << 22;
			check_reserved_registers(harness, encoded, seed);
		}
	for (const auto operation : test::standard_operations) {
		auto encoded = test::instruction(
			{operation, false, 64U, 16U});
		encoded &= ~0x4000'0000U;
		check_reserved_registers(harness, encoded, seed);
	}
	for (const auto operation : test::sized_operations) {
		auto encoded = test::instruction(
			{operation, false, 64U, 16U});
		encoded &= ~0x4000'0000U;
		check_reserved_registers(harness, encoded, seed);
	}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	every_valid_register_encoding(harness);
	every_reserved_precision_and_arrangement(harness);
	harness.print_summary("arm64-floating-round-allocation");
	return 0;
}
