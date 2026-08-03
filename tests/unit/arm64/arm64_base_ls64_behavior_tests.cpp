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

void check_success(test::EngineHarness &harness, ls64::Operation operation,
	unsigned first, unsigned base, unsigned status, unsigned salt,
	std::size_t &cases)
{
	ls64::Page initial_memory{};
	ls64::fill_pattern(initial_memory, salt);
	auto expected_memory = initial_memory;

	auto initial = ls64::initial_state();
	ls64::set_base(
		initial, base, ls64::data_base + ls64::transfer_offset);
	auto expected = initial;
	if (ls64::is_load(operation))
		ls64::apply_load_oracle(expected, first, initial_memory);
	else
		ls64::apply_store_oracle(
			expected_memory, expected, initial, operation, first, status);

	const std::array regions{
		ls64::memory_region(initial_memory, expected_memory),
	};
	const ls64::CaseName name{"ls64-behavior-op%u-t%u-n%u-s%u",
		static_cast<unsigned>(operation), first, base, status};
	harness.check({
		.name = name,
		.instruction = ls64::encode(operation, first, base, status),
		.initial = initial,
		.expected = expected,
		.native = ls64::native_requirements(operation),
		.memory = regions,
	});
	++cases;
}

void ordinary_and_alias_behavior(
	test::EngineHarness &harness, std::size_t &cases)
{
	unsigned salt = 1;
	for (const auto operation : operations)
		for (const unsigned first : {0U, 22U})
			for (const unsigned base : {3U, 31U}) {
				const unsigned status_count =
					ls64::has_status(operation) ? 2U : 1U;
				for (unsigned status_index = 0;
				     status_index < status_count; ++status_index) {
					const unsigned status =
						ls64::has_status(operation) &&
							status_index == 0 ?
						7U : 31U;
					check_success(harness, operation, first, base,
						status, salt++, cases);
				}
			}
}

void status_base_aliases(test::EngineHarness &harness, std::size_t &cases)
{
	unsigned salt = 0x41;
	for (const auto operation :
	     {ls64::Operation::store_status,
	      ls64::Operation::store_status_accdata})
		/* X9 supplies the address and receives status only after the
		 * complete transfer. X10-X17 supply the stored payload.
		 */
		check_success(harness, operation, 10, 9, 9, salt++, cases);
}

void matching_allocation_tags(
	test::EngineHarness &harness, std::size_t &cases)
{
	constexpr std::uint8_t tag = 5;
	const std::array initial_tags{
		test::AllocationTag{.offset = ls64::transfer_offset, .value = tag},
		test::AllocationTag{.offset = ls64::transfer_offset + 16, .value = tag},
		test::AllocationTag{.offset = ls64::transfer_offset + 32, .value = tag},
		test::AllocationTag{.offset = ls64::transfer_offset + 48, .value = tag},
	};
	unsigned salt = 0x71;
	for (const auto operation : operations) {
		ls64::Page initial_memory{};
		ls64::fill_pattern(initial_memory, salt++);
		auto expected_memory = initial_memory;
		auto initial = ls64::initial_state();
		initial.pstate_tco = false;
		const auto tagged_address =
			(ls64::data_base + ls64::transfer_offset) |
			(static_cast<std::uint64_t>(tag) << 56);
		ls64::set_base(initial, 9, tagged_address);
		auto expected = initial;
		const unsigned status =
			ls64::has_status(operation) ? 8U : 31U;
		if (ls64::is_load(operation))
			ls64::apply_load_oracle(expected, 0, initial_memory);
		else
			ls64::apply_store_oracle(expected_memory, expected,
				initial, operation, 0, status);
		const std::array regions{ls64::memory_region(
			initial_memory, expected_memory, true, true, initial_tags)};
		const ls64::CaseName name{"ls64-matching-tags-op%u",
			static_cast<unsigned>(operation)};
		harness.check({
			.name = name,
			.instruction = ls64::encode(operation, 0, 9, status),
			.initial = initial,
			.expected = expected,
			.native = ls64::native_requirements(operation),
			.memory = regions,
		});
		++cases;
	}
}

void stack_pointer_bypasses_tag_check(
	test::EngineHarness &harness, std::size_t &cases)
{
	const std::array initial_tags{
		test::AllocationTag{.offset = ls64::transfer_offset, .value = 9},
		test::AllocationTag{.offset = ls64::transfer_offset + 16, .value = 8},
		test::AllocationTag{.offset = ls64::transfer_offset + 32, .value = 7},
		test::AllocationTag{.offset = ls64::transfer_offset + 48, .value = 6},
	};
	unsigned salt = 0x81;
	for (const auto operation : operations) {
		ls64::Page initial_memory{};
		ls64::fill_pattern(initial_memory, salt++);
		auto expected_memory = initial_memory;
		auto initial = ls64::initial_state();
		ls64::set_base(
			initial, 31, ls64::data_base + ls64::transfer_offset);
		auto expected = initial;
		const unsigned status =
			ls64::has_status(operation) ? 8U : 31U;
		if (ls64::is_load(operation))
			ls64::apply_load_oracle(expected, 0, initial_memory);
		else
			ls64::apply_store_oracle(expected_memory, expected,
				initial, operation, 0, status);
		const std::array regions{ls64::memory_region(
			initial_memory, expected_memory, true, true, initial_tags)};
		const ls64::CaseName name{"ls64-sp-no-tag-check-op%u",
			static_cast<unsigned>(operation)};
		harness.check({
			.name = name,
			.instruction = ls64::encode(operation, 0, 31, status),
			.initial = initial,
			.expected = expected,
			.native = ls64::native_requirements(operation),
			.memory = regions,
		});
		++cases;
	}
}

void tag_check_override_bypasses_tag_check(
	test::EngineHarness &harness, std::size_t &cases)
{
	constexpr std::uint8_t logical_tag = 5;
	const std::array initial_tags{
		test::AllocationTag{
			.offset = ls64::transfer_offset,
			.value = logical_tag + 1,
		},
		test::AllocationTag{
			.offset = ls64::transfer_offset + 16,
			.value = logical_tag + 2,
		},
		test::AllocationTag{
			.offset = ls64::transfer_offset + 32,
			.value = logical_tag + 3,
		},
		test::AllocationTag{
			.offset = ls64::transfer_offset + 48,
			.value = logical_tag + 4,
		},
	};
	unsigned salt = 0x91;
	for (const auto operation : operations) {
		ls64::Page initial_memory{};
		ls64::fill_pattern(initial_memory, salt++);
		auto expected_memory = initial_memory;
		auto initial = ls64::initial_state();
		initial.pstate_tco = true;
		const auto tagged_address =
			(ls64::data_base + ls64::transfer_offset) |
			(static_cast<std::uint64_t>(logical_tag) << 56);
		ls64::set_base(initial, 9, tagged_address);
		auto expected = initial;
		const unsigned status =
			ls64::has_status(operation) ? 8U : 31U;
		if (ls64::is_load(operation))
			ls64::apply_load_oracle(expected, 0, initial_memory);
		else
			ls64::apply_store_oracle(expected_memory, expected,
				initial, operation, 0, status);
		const std::array regions{ls64::memory_region(
			initial_memory, expected_memory, true, true, initial_tags)};
		const ls64::CaseName name{"ls64-tag-check-override-op%u",
			static_cast<unsigned>(operation)};
		harness.check({
			.name = name,
			.instruction = ls64::encode(operation, 0, 9, status),
			.initial = initial,
			.expected = expected,
			.native = ls64::native_requirements(operation),
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
	ordinary_and_alias_behavior(harness, cases);
	status_base_aliases(harness, cases);
	matching_allocation_tags(harness, cases);
	stack_pointer_bypasses_tag_check(harness, cases);
	tag_check_override_bypasses_tag_check(harness, cases);
	ls64::require(cases == 38);
	ls64::require_counts(harness, cases);
	harness.print_summary("arm64-base-ls64-behavior");
}
