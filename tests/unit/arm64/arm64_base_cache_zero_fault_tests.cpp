// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_cache_zero_test_support.hpp"

#include <span>

namespace test = arm64_base_cache_zero_test;

namespace {

void unmapped_faults(test::engine_tests::EngineHarness &harness)
{
	for (const unsigned source : {7U, 31U}) {
		auto initial = test::state(source);
		if (source != 31U)
			initial.registers[source] =
				test::engine_tests::engine_test_data_base +
				test::block_offset + 17;
		const auto address = source == 31U ? 0 :
			test::engine_tests::engine_test_data_base +
			test::block_offset;
		harness.check({
			.name = "cache-zero unmapped fault",
			.instruction = test::instruction(source),
			.initial = initial,
			.expected = initial,
			.event = test::engine_tests::ExpectedEvent::memory_fault,
			.native = test::native_requirements(),
			.fault =
				{
					.address = address,
					.access = test::arm64::MemoryAccess::write,
				},
			.compare_fault = true,
		});
	}
}

void permission_fault_is_atomic(test::engine_tests::EngineHarness &harness)
{
	const auto initial_page = test::filled_page();
	const test::engine_tests::MemoryRegion region{
		.address = test::engine_tests::engine_test_data_base,
		.initial = initial_page,
		.readable = true,
		.writable = false,
		.guest_writable = false,
	};
	auto initial = test::state(91);
	initial.registers[9] = test::engine_tests::engine_test_data_base +
		test::block_offset + 63;
	harness.check({
		.name = "cache-zero permission fault",
		.instruction = test::instruction(9),
		.initial = initial,
		.expected = initial,
		.event = test::engine_tests::ExpectedEvent::memory_fault,
		.native = test::native_requirements(),
		.fault =
			{
				.address = test::engine_tests::engine_test_data_base +
					test::block_offset,
				.access = test::arm64::MemoryAccess::write,
			},
		.compare_fault = true,
		.memory = std::span<const test::engine_tests::MemoryRegion>{
			&region, 1},
	});
}

void write_only_mapping_succeeds(test::engine_tests::EngineHarness &harness)
{
	const auto initial_page = test::filled_page();
	const auto expected_page = test::zeroed_page(test::block_offset);
	const test::engine_tests::MemoryRegion region{
		.address = test::engine_tests::engine_test_data_base,
		.initial = initial_page,
		.expected = expected_page,
		.readable = false,
		.writable = true,
	};
	auto initial = test::state(92);
	initial.registers[10] = test::engine_tests::engine_test_data_base +
		test::block_offset + 31;
	harness.check({
		.name = "cache-zero write-only mapping",
		.instruction = test::instruction(10),
		.initial = initial,
		.expected = initial,
		.native = test::native_requirements(),
		.memory = std::span<const test::engine_tests::MemoryRegion>{
			&region, 1},
	});
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	unmapped_faults(harness);
	permission_fault_is_atomic(harness);
	write_only_mapping_succeeds(harness);
	harness.print_summary("arm64-base-cache-zero-faults");
	return 0;
}
