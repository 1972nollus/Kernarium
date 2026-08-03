// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_gcs_test_support.hpp"

#include <array>

namespace gcs = arm64_base_gcs_test;
namespace test = uml::emulator::tests;

namespace {

void switch_stack_1_all_transfers(test::EngineHarness &harness,
	std::size_t &cases)
{
	for (unsigned transfer = 0; transfer < 32; ++transfer) {
		auto initial = gcs::initial_state();
		const auto outgoing = initial.gcs_pointer;
		if (transfer == 31U) {
			harness.check({
				.name = "gcsss1-xzr-unmapped-zero",
				.instruction =
					gcs::encode(gcs::Operation::switch_stack_1, transfer),
				.initial = initial,
				.expected = initial,
				.event = test::ExpectedEvent::memory_fault,
				.native = gcs::native_gcs,
				.fault = {
					.address = 0,
					.access = gcs::arm64::MemoryAccess::write,
				},
				.compare_fault = true,
			});
			++cases;
			continue;
		}

		gcs::Page initial_memory;
		gcs::fill_pattern(initial_memory, 91U + transfer);
		auto expected_memory = initial_memory;
		const auto incoming =
			(gcs::data_base + gcs::page_size / 4 +
			 static_cast<std::uint64_t>(transfer) * 8) &
			~std::uint64_t{7};
		initial.registers[transfer] = incoming;
		const auto compare =
			(incoming & ~std::uint64_t{0xfff}) | 1U;
		const auto replacement =
			(outgoing & ~std::uint64_t{7}) | 5U;
		const auto offset =
			static_cast<std::size_t>(incoming - gcs::data_base);
		gcs::store_u64(initial_memory, offset, compare);
		gcs::store_u64(expected_memory, offset, replacement);
		auto expected = initial;
		expected.gcs_pointer = incoming & ~std::uint64_t{7};
		const std::array regions{
			gcs::memory_region(initial_memory, expected_memory),
		};
		const gcs::CaseName name{"gcsss1-t%u", transfer};
		harness.check({
			.name = name,
			.instruction =
				gcs::encode(gcs::Operation::switch_stack_1, transfer),
			.initial = initial,
			.expected = expected,
			.native = gcs::native_gcs,
			.memory = regions,
		});
		++cases;
	}
}

void switch_stack_2_all_transfers(test::EngineHarness &harness,
	std::size_t &cases)
{
	for (unsigned transfer = 0; transfer < 32; ++transfer) {
		gcs::Page initial_memory;
		gcs::fill_pattern(initial_memory, 151U + transfer);
		auto expected_memory = initial_memory;
		auto initial = gcs::initial_state();
		const auto incoming = initial.gcs_pointer;
		const auto outgoing =
			(gcs::data_base + gcs::page_size / 4) &
			~std::uint64_t{7};
		const auto token = (outgoing + 8) | 5U;
		gcs::store_u64(initial_memory,
			static_cast<std::size_t>(incoming - gcs::data_base), token);
		expected_memory = initial_memory;
		gcs::store_u64(expected_memory,
			static_cast<std::size_t>(outgoing - gcs::data_base),
			(outgoing & ~std::uint64_t{0xfff}) | 1U);
		auto expected = initial;
		expected.gcs_pointer = (incoming + 8) & ~std::uint64_t{7};
		gcs::write_register(expected, transfer, outgoing, false);
		const std::array regions{
			gcs::memory_region(initial_memory, expected_memory),
		};
		const gcs::CaseName name{"gcsss2-t%u", transfer};
		harness.check({
			.name = name,
			.instruction =
				gcs::encode(gcs::Operation::switch_stack_2, transfer),
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
	switch_stack_1_all_transfers(harness, cases);
	switch_stack_2_all_transfers(harness, cases);
	gcs::require(cases == 64);
	gcs::require_counts(harness, cases);
	harness.print_summary("arm64-base-gcs-control");
}
