// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_gcs_test_support.hpp"

#include <array>

namespace gcs = arm64_base_gcs_test;
namespace test = uml::backend::tests;

namespace {

void pop_main_all_transfers(test::EngineHarness &harness,
	std::size_t &cases)
{
	for (unsigned transfer = 0; transfer < 32; ++transfer) {
		gcs::Page initial_memory;
		gcs::fill_pattern(initial_memory, transfer);
		auto initial = gcs::initial_state();
		const auto offset = static_cast<std::size_t>(
			initial.gcs_pointer - gcs::data_base);
		const auto value =
			0x2244'6688'aacc'0000ULL +
			static_cast<std::uint64_t>(transfer) * 4;
		gcs::store_u64(initial_memory, offset, value);
		const auto expected_memory = initial_memory;
		auto expected = initial;
		expected.gcs_pointer = (initial.gcs_pointer + 8) &
			~std::uint64_t{7};
		gcs::write_register(expected, transfer, value, false);
		const std::array regions{
			gcs::memory_region(initial_memory, expected_memory),
		};
		const gcs::CaseName name{"gcspopm-t%u", transfer};
		harness.check({
			.name = name,
			.instruction =
				gcs::encode(gcs::Operation::pop_main, transfer),
			.initial = initial,
			.expected = expected,
			.native = gcs::native_gcs,
			.memory = regions,
		});
		++cases;
	}
}

void push_main_all_transfers(test::EngineHarness &harness,
	std::size_t &cases)
{
	for (unsigned transfer = 0; transfer < 32; ++transfer) {
		gcs::Page initial_memory;
		gcs::fill_pattern(initial_memory, 47U + transfer);
		auto expected_memory = initial_memory;
		auto initial = gcs::initial_state();
		const auto address = initial.gcs_pointer - 8;
		const auto value = gcs::read_register(initial, transfer, false);
		gcs::store_u64(expected_memory,
			static_cast<std::size_t>(address - gcs::data_base), value);
		auto expected = initial;
		expected.gcs_pointer = address & ~std::uint64_t{7};
		const std::array regions{
			gcs::memory_region(initial_memory, expected_memory),
		};
		const gcs::CaseName name{"gcspushm-t%u", transfer};
		harness.check({
			.name = name,
			.instruction =
				gcs::encode(gcs::Operation::push_main, transfer),
			.initial = initial,
			.expected = expected,
			.native = gcs::native_gcs,
			.memory = regions,
		});
		++cases;
	}
}

} // namespace

int main()
{
	test::EngineHarness harness;
	std::size_t cases = 0;
	pop_main_all_transfers(harness, cases);
	push_main_all_transfers(harness, cases);
	gcs::require(cases == 64);
	gcs::require_counts(harness, cases);
	harness.print_summary("arm64-base-gcs-behavior");
}
