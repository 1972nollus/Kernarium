// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_ls64_test_support.hpp"

#include <array>

namespace ls64 = arm64_base_ls64_test;
namespace test = uml::emulator::tests;

namespace {

constexpr std::array operations{
	ls64::Operation::load,
	ls64::Operation::store,
	ls64::Operation::store_status,
	ls64::Operation::store_status_accdata,
};

[[nodiscard]] constexpr unsigned status_for(
	ls64::Operation operation) noexcept
{
	return ls64::has_status(operation) ? 6U : 31U;
}

void check_fault(test::EngineHarness &harness, std::string_view name,
	ls64::Operation operation, unsigned first, unsigned base,
	std::uint64_t address, std::span<const test::MemoryRegion> memory,
	std::size_t &cases)
{
	auto initial = ls64::initial_state();
	ls64::set_base(initial, base, address);
	harness.check({
		.name = name,
		.instruction = ls64::encode(
			operation, first, base, status_for(operation)),
		.initial = initial,
		.expected = initial,
		.event = test::ExpectedEvent::memory_fault,
		.native = ls64::native_requirements(operation),
		.fault = {
			.address = address,
			.access = ls64::is_load(operation) ?
				ls64::arm64::MemoryAccess::read :
				ls64::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
		.memory = memory,
	});
	++cases;
}

void alignment_faults(test::EngineHarness &harness, std::size_t &cases)
{
	ls64::Page page{};
	ls64::fill_pattern(page, 0x51);
	const std::array regions{ls64::memory_region(page, page)};
	for (const auto operation : operations) {
		const ls64::CaseName gpr_name{
			"ls64-fault-gpr-alignment-op%u",
			static_cast<unsigned>(operation)};
		check_fault(harness, gpr_name, operation, 4, 5,
			ls64::data_base + ls64::transfer_offset + 1,
			regions, cases);

		const ls64::CaseName sp_name{
			"ls64-fault-sp-64byte-alignment-op%u",
			static_cast<unsigned>(operation)};
		/* This value satisfies the ordinary 16-byte SP check but not the
		 * instruction's mandatory 64-byte transfer alignment.
		 */
		check_fault(harness, sp_name, operation, 4, 31,
			ls64::data_base + ls64::transfer_offset + 16,
			regions, cases);
	}
}

void permission_faults(test::EngineHarness &harness, std::size_t &cases)
{
	ls64::Page page{};
	ls64::fill_pattern(page, 0x62);
	const auto address = ls64::data_base + ls64::transfer_offset;
	for (const auto operation : operations) {
		const bool load = ls64::is_load(operation);
		const std::array regions{
			ls64::memory_region(page, page, !load, load),
		};
		const ls64::CaseName name{"ls64-fault-permission-op%u",
			static_cast<unsigned>(operation)};
		check_fault(
			harness, name, operation, 22, 31, address, regions, cases);
	}
}

void unmapped_faults(test::EngineHarness &harness, std::size_t &cases)
{
	const auto address = ls64::data_base;
	for (const auto operation : operations) {
		const ls64::CaseName name{"ls64-fault-unmapped-op%u",
			static_cast<unsigned>(operation)};
		check_fault(harness, name, operation, 20, 2, address, {}, cases);
	}
}

void reserved_encoding_precedes_faults(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const auto operation : operations) {
		auto initial = ls64::initial_state();
		ls64::set_base(initial, 1, ls64::data_base + 1);
		const ls64::CaseName name{"ls64-fault-reserved-first-op%u",
			static_cast<unsigned>(operation)};
		harness.check_illegal(name,
			ls64::encode(
				operation, 31, 1, status_for(operation)),
			initial, ls64::native_requirements(operation));
		++cases;
	}
}

void allocation_tag_faults(test::EngineHarness &harness, std::size_t &cases)
{
	constexpr std::uint8_t logical_tag = 5;
	const std::array initial_tags{
		test::AllocationTag{
			.offset = ls64::transfer_offset,
			.value = logical_tag + 1,
		},
		test::AllocationTag{
			.offset = ls64::transfer_offset + 16,
			.value = logical_tag,
		},
		test::AllocationTag{
			.offset = ls64::transfer_offset + 32,
			.value = logical_tag,
		},
		test::AllocationTag{
			.offset = ls64::transfer_offset + 48,
			.value = logical_tag,
		},
	};
	ls64::Page page{};
	ls64::fill_pattern(page, 0x73);
	const std::array regions{ls64::memory_region(
		page, page, true, true, initial_tags)};
	const auto address = ls64::data_base + ls64::transfer_offset;
	const auto tagged_address =
		address | (static_cast<std::uint64_t>(logical_tag) << 56);
	for (const auto operation : operations) {
		auto initial = ls64::initial_state();
		initial.pstate_tco = false;
		ls64::set_base(initial, 9, tagged_address);
		const ls64::CaseName name{"ls64-fault-allocation-tag-op%u",
			static_cast<unsigned>(operation)};
		harness.check({
			.name = name,
			.instruction = ls64::encode(
				operation, 4, 9, status_for(operation)),
			.initial = initial,
			.expected = initial,
			.event = test::ExpectedEvent::memory_fault,
			.native = ls64::native_requirements(operation),
			.fault = {
				.address = address,
				.access = ls64::is_load(operation) ?
					ls64::arm64::MemoryAccess::read :
					ls64::arm64::MemoryAccess::write,
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
	alignment_faults(harness, cases);
	permission_faults(harness, cases);
	unmapped_faults(harness, cases);
	reserved_encoding_precedes_faults(harness, cases);
	allocation_tag_faults(harness, cases);
	ls64::require(cases == 24);
	ls64::require_counts(harness, cases);
	harness.print_summary("arm64-base-ls64-fault");
}
