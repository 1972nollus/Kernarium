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

void encoding_examples()
{
	ls64::require(ls64::encode(
		ls64::Operation::load, 0, 1) == 0xf83f'd020U);
	ls64::require(ls64::encode(
		ls64::Operation::load, 22, 31) == 0xf83f'd3f6U);
	ls64::require(ls64::encode(
		ls64::Operation::store, 0, 1) == 0xf83f'9020U);
	ls64::require(ls64::encode(
		ls64::Operation::store, 22, 31) == 0xf83f'93f6U);
	ls64::require(ls64::encode(
		ls64::Operation::store_status, 0, 1, 2) == 0xf822'b020U);
	ls64::require(ls64::encode(
		ls64::Operation::store_status, 22, 31, 31) == 0xf83f'b3f6U);
	ls64::require(ls64::encode(
		ls64::Operation::store_status_accdata,
		0, 1, 2) == 0xf822'a020U);
	ls64::require(ls64::encode(
		ls64::Operation::store_status_accdata,
		22, 31, 31) == 0xf83f'a3f6U);

	constexpr std::array expected_features{
		::uml::cpu::ArmFeature::ls64,
		::uml::cpu::ArmFeature::ls64,
		::uml::cpu::ArmFeature::ls64_v,
		::uml::cpu::ArmFeature::ls64_accdata,
	};
	for (std::size_t index = 0; index < operations.size(); ++index) {
		const auto required =
			ls64::native_requirements(operations[index]).features();
		ls64::require(required.count() == 1);
		ls64::require(required.test(
			static_cast<std::size_t>(expected_features[index])));
	}
}

void every_allocated_field_combination(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const auto operation : operations)
		for (unsigned first = 0; first < 24; first += 2)
			for (unsigned base = 0; base < 32; ++base) {
				const unsigned status_count =
					ls64::has_status(operation) ? 32U : 1U;
				for (unsigned status_index = 0;
				     status_index < status_count; ++status_index) {
					const unsigned status =
						ls64::has_status(operation) ?
							status_index : 31U;
					auto initial = ls64::initial_state();
					ls64::set_base(initial, base, ls64::data_base);
					const ls64::CaseName name{
						"ls64-allocated-op%u-t%u-n%u-s%u",
						static_cast<unsigned>(operation), first,
						base, status};
					harness.check({
						.name = name,
						.instruction = ls64::encode(
							operation, first, base, status),
						.initial = initial,
						.expected = initial,
						.event = test::ExpectedEvent::memory_fault,
						.native =
							ls64::native_requirements(operation),
						.fault = {
							.address = ls64::data_base,
							.access = ls64::is_load(operation) ?
								ls64::arm64::MemoryAccess::read :
								ls64::arm64::MemoryAccess::write,
						},
						.compare_fault = true,
					});
					++cases;
				}
			}
}

void every_reserved_first_register_combination(
	test::EngineHarness &harness, std::size_t &cases)
{
	for (const auto operation : operations)
		for (unsigned first = 0; first < 32; ++first) {
			if (ls64::valid_first(first))
				continue;
			for (unsigned base = 0; base < 32; ++base) {
				const unsigned status_count =
					ls64::has_status(operation) ? 32U : 1U;
				for (unsigned status_index = 0;
				     status_index < status_count; ++status_index) {
					const unsigned status =
						ls64::has_status(operation) ?
							status_index : 31U;
					auto initial = ls64::initial_state();
					/* A reserved Rt must win over both address
					 * misalignment and a missing mapping.
					 */
					ls64::set_base(
						initial, base, ls64::data_base + 1);
					const ls64::CaseName name{
						"ls64-reserved-op%u-t%u-n%u-s%u",
						static_cast<unsigned>(operation), first,
						base, status};
					harness.check_illegal(name,
						ls64::encode(
							operation, first, base, status),
						initial,
						ls64::native_requirements(operation));
					++cases;
				}
			}
		}
}

} // namespace

int main()
{
	encoding_examples();
	test::EngineHarness harness;
	std::size_t allocated_cases = 0;
	std::size_t reserved_cases = 0;
	every_allocated_field_combination(harness, allocated_cases);
	every_reserved_first_register_combination(harness, reserved_cases);
	ls64::require(allocated_cases == 25'344);
	ls64::require(reserved_cases == 42'240);
	ls64::require_counts(harness, allocated_cases + reserved_cases);
	harness.print_summary("arm64-base-ls64-allocation");
}
