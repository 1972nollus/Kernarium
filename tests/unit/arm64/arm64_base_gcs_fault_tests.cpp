// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_gcs_test_support.hpp"

#include <array>

namespace gcs = arm64_base_gcs_test;
namespace test = uml::emulator::tests;

namespace {

static_assert(static_cast<int>(uml::cpu::EngineEvent::gcs_fault) ==
	DARWIN_UML_EVENT_GCS_FAULT);
static_assert(DARWIN_UML_EVENT_GCS_FAULT == 6);

void access_checks_precede_state_and_memory(test::EngineHarness &harness,
	std::size_t &cases)
{
	{
		auto initial = gcs::initial_state();
		initial.gcs_implemented = false;
		initial.gcs_pointer = gcs::data_base + 3 * gcs::page_size;
		gcs::check_allocated_illegal(harness,
			"gcspopm-feature-before-memory",
			gcs::encode(gcs::Operation::pop_main, 1),
			initial);
		++cases;
	}
	{
		auto initial = gcs::initial_state();
		initial.gcs_push_enabled = false;
		initial.gcs_pointer = gcs::data_base + 3 * gcs::page_size;
		gcs::check_allocated_illegal(harness,
			"gcspushm-access-before-source-memory",
			gcs::encode(gcs::Operation::push_main, 31),
			initial);
		++cases;
	}
	for (const auto operation :
	     {gcs::Operation::store, gcs::Operation::store_unprivileged}) {
		auto initial = gcs::initial_state();
		initial.gcs_store_enabled = false;
		initial.sp = gcs::data_base + 1;
		const gcs::CaseName name{"%s-enable-before-sp-alignment",
			operation == gcs::Operation::store ?
				"gcsstr" : "gcssttr"};
		harness.check({
			.name = name,
			.instruction = gcs::encode(operation, 31, 31),
			.initial = initial,
			.expected = initial,
			.event = test::ExpectedEvent::gcs_fault,
			.native = gcs::native_gcs,
		});
		++cases;
	}
}

void pop_faults(test::EngineHarness &harness, std::size_t &cases)
{
	{
		auto initial = gcs::initial_state();
		initial.gcs_pointer = gcs::data_base + 2 * gcs::page_size;
		harness.check({
			.name = "gcspopm-unmapped",
			.instruction = gcs::encode(gcs::Operation::pop_main, 2),
			.initial = initial,
			.expected = initial,
			.event = test::ExpectedEvent::memory_fault,
			.native = gcs::native_gcs,
			.fault = {
				.address = initial.gcs_pointer,
				.access = gcs::arm64::MemoryAccess::read,
			},
			.compare_fault = true,
		});
		++cases;
	}
	{
		gcs::Page initial_memory;
		gcs::fill_pattern(initial_memory, 203);
		auto initial = gcs::initial_state();
		const auto offset = static_cast<std::size_t>(
			initial.gcs_pointer - gcs::data_base);
		gcs::store_u64(initial_memory, offset, 0x1234'5678'9abc'def3ULL);
		const auto unchanged = initial_memory;
		const std::array regions{
			gcs::memory_region(initial_memory, unchanged),
		};
		harness.check({
			.name = "gcspopm-data-check-before-writeback",
			.instruction = gcs::encode(gcs::Operation::pop_main, 3),
			.initial = initial,
			.expected = initial,
			.event = test::ExpectedEvent::gcs_fault,
			.native = gcs::native_gcs,
			.memory = regions,
		});
		++cases;
	}
}

void push_faults(test::EngineHarness &harness, std::size_t &cases)
{
	gcs::Page initial_memory;
	gcs::fill_pattern(initial_memory, 211);
	const auto expected_memory = initial_memory;
	auto initial = gcs::initial_state();
	const std::array regions{
		gcs::memory_region(initial_memory, expected_memory, true, false),
	};
	harness.check({
		.name = "gcspushm-read-only-no-writeback",
		.instruction = gcs::encode(gcs::Operation::push_main, 4),
		.initial = initial,
		.expected = initial,
		.event = test::ExpectedEvent::memory_fault,
		.native = gcs::native_gcs,
		.fault = {
			.address = initial.gcs_pointer - 8,
			.access = gcs::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
		.memory = regions,
	});
	++cases;
}

void switch_stack_faults(test::EngineHarness &harness,
	std::size_t &cases)
{
	{
		gcs::Page initial_memory;
		gcs::fill_pattern(initial_memory, 223);
		const auto expected_memory = initial_memory;
		auto initial = gcs::initial_state();
		initial.registers[5] = gcs::data_base + 0x101;
		const std::array regions{
			gcs::memory_region(initial_memory, expected_memory),
		};
		harness.check({
			.name = "gcsss1-unaligned-atomic",
			.instruction =
				gcs::encode(gcs::Operation::switch_stack_1, 5),
			.initial = initial,
			.expected = initial,
			.event = test::ExpectedEvent::memory_fault,
			.native = gcs::native_gcs,
			.fault = {
				.address = initial.registers[5],
				.access = gcs::arm64::MemoryAccess::write,
			},
			.compare_fault = true,
			.memory = regions,
		});
		++cases;
	}
	{
		gcs::Page initial_memory;
		gcs::fill_pattern(initial_memory, 229);
		auto initial = gcs::initial_state();
		const auto incoming =
			(gcs::data_base + gcs::page_size / 4) &
			~std::uint64_t{7};
		initial.registers[6] = incoming;
		gcs::store_u64(initial_memory,
			static_cast<std::size_t>(incoming - gcs::data_base),
			((incoming & ~std::uint64_t{0xfff}) | 1U) ^ 8U);
		const auto expected_memory = initial_memory;
		const std::array regions{
			gcs::memory_region(initial_memory, expected_memory),
		};
		harness.check({
			.name = "gcsss1-compare-data-check",
			.instruction =
				gcs::encode(gcs::Operation::switch_stack_1, 6),
			.initial = initial,
			.expected = initial,
			.event = test::ExpectedEvent::gcs_fault,
			.native = gcs::native_gcs,
			.memory = regions,
		});
		++cases;
	}
	{
		gcs::Page initial_memory;
		gcs::fill_pattern(initial_memory, 233);
		auto initial = gcs::initial_state();
		gcs::store_u64(initial_memory,
			static_cast<std::size_t>(
				initial.gcs_pointer - gcs::data_base),
			0x1122'3344'5566'7781ULL);
		const auto expected_memory = initial_memory;
		const std::array regions{
			gcs::memory_region(initial_memory, expected_memory),
		};
		harness.check({
			.name = "gcsss2-invalid-token-before-store",
			.instruction =
				gcs::encode(gcs::Operation::switch_stack_2, 7),
			.initial = initial,
			.expected = initial,
			.event = test::ExpectedEvent::gcs_fault,
			.native = gcs::native_gcs,
			.memory = regions,
		});
		++cases;
	}
	{
		gcs::Page initial_memory;
		gcs::fill_pattern(initial_memory, 239);
		auto initial = gcs::initial_state();
		const auto outgoing =
			gcs::data_base + 2 * gcs::page_size;
		gcs::store_u64(initial_memory,
			static_cast<std::size_t>(
				initial.gcs_pointer - gcs::data_base),
			(outgoing + 8) | 5U);
		const auto expected_memory = initial_memory;
		const std::array regions{
			gcs::memory_region(initial_memory, expected_memory),
		};
		harness.check({
			.name = "gcsss2-store-fault-restores-pointer-destination",
			.instruction =
				gcs::encode(gcs::Operation::switch_stack_2, 8),
			.initial = initial,
			.expected = initial,
			.event = test::ExpectedEvent::memory_fault,
			.native = gcs::native_gcs,
			.fault = {
				.address = outgoing,
				.access = gcs::arm64::MemoryAccess::write,
			},
			.compare_fault = true,
			.memory = regions,
		});
		++cases;
	}
}

void direct_store_faults(test::EngineHarness &harness,
	std::size_t &cases)
{
	for (const auto operation :
	     {gcs::Operation::store, gcs::Operation::store_unprivileged}) {
		auto initial = gcs::initial_state();
		initial.sp = gcs::data_base + 1;
		const gcs::CaseName name{"%s-sp-alignment-before-source",
			operation == gcs::Operation::store ?
				"gcsstr" : "gcssttr"};
		harness.check({
			.name = name,
			.instruction = gcs::encode(operation, 31, 31),
			.initial = initial,
			.expected = initial,
			.event = test::ExpectedEvent::memory_fault,
			.native = gcs::native_gcs,
			.fault = {
				.address = initial.sp,
				.access = gcs::arm64::MemoryAccess::write,
			},
			.compare_fault = true,
		});
		++cases;
	}
	for (const auto operation :
	     {gcs::Operation::store, gcs::Operation::store_unprivileged}) {
		gcs::Page initial_memory;
		gcs::fill_pattern(initial_memory, 251);
		const auto expected_memory = initial_memory;
		auto initial = gcs::initial_state();
		initial.registers[9] =
			gcs::data_base + gcs::page_size - 4;
		const std::array regions{
			gcs::memory_region(initial_memory, expected_memory),
		};
		const gcs::CaseName name{"%s-cross-page-store-is-atomic",
			operation == gcs::Operation::store ?
				"gcsstr" : "gcssttr"};
		harness.check({
			.name = name,
			.instruction = gcs::encode(operation, 10, 9),
			.initial = initial,
			.expected = initial,
			.event = test::ExpectedEvent::memory_fault,
			.native = gcs::native_gcs,
			.fault = {
				.address = gcs::data_base + gcs::page_size,
				.access = gcs::arm64::MemoryAccess::write,
			},
			.compare_fault = true,
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
	access_checks_precede_state_and_memory(harness, cases);
	pop_faults(harness, cases);
	push_faults(harness, cases);
	switch_stack_faults(harness, cases);
	direct_store_faults(harness, cases);
	gcs::require(cases == 15);
	gcs::require_counts(harness, cases);
	harness.print_summary("arm64-base-gcs-fault");
}
