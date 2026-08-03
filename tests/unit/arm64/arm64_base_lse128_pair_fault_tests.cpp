// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_lse128_pair_test_support.hpp"

#include <array>
#include <cstdio>

namespace pair128 = arm64_base_lse128_pair_test;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;

namespace {

constexpr std::array operations{
	pair128::Operation::clear,
	pair128::Operation::set,
	pair128::Operation::swap,
};

void check_fault(
	engine::EngineHarness &harness, pair128::ExpectedCounts &counts,
	std::string_view name, pair128::Operation operation, bool acquire,
	bool release, unsigned rn, std::uint64_t address, bool mapped,
	bool writable)
{
	auto initial = atomic::state(
		0x6000U + pair128::code(operation) * 16U +
		static_cast<unsigned>(acquire) * 2U +
		static_cast<unsigned>(release));
	pair128::write_pair(initial, 3, 4, pair128::operand);
	atomic::write_register(initial, rn, address, true);
	atomic::Page page{};
	for (std::size_t index = 0; index < page.size(); ++index)
		page[index] = static_cast<std::byte>((index * 29U + 7U) & 0xffU);
	const auto memory = atomic::region(page, page, true, writable);
	const auto native = atomic::lse128();
	harness.check({
		.name = name,
		.instruction = pair128::instruction(
			operation, acquire, release, rn, 3, 4),
		.initial = initial,
		.expected = initial,
		.event = engine::ExpectedEvent::memory_fault,
		.native = native,
		.fault = {
			.address = address,
			.access = pair128::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
		.memory = mapped ?
			std::span<const engine::MemoryRegion>{&memory, 1} :
			std::span<const engine::MemoryRegion>{},
	});
	counts.add(native);
}

void alignment_faults(
	engine::EngineHarness &harness, pair128::ExpectedCounts &counts)
{
	for (const auto operation : operations)
		for (const bool acquire : {false, true})
			for (const bool release : {false, true})
				for (const unsigned rn : {2U, 31U}) {
					char name[128];
					std::snprintf(name, sizeof(name),
						"operation=%u misaligned %s A=%u R=%u",
						pair128::code(operation), rn == 31U ? "sp" : "base",
						static_cast<unsigned>(acquire),
						static_cast<unsigned>(release));
					check_fault(harness, counts, name, operation, acquire,
						release, rn, atomic::data_address + 8U, true, true);
				}
}

void mapping_and_permission_faults(
	engine::EngineHarness &harness, pair128::ExpectedCounts &counts)
{
	for (const auto operation : operations) {
		char unmapped_name[96];
		std::snprintf(unmapped_name, sizeof(unmapped_name),
			"operation=%u unmapped", pair128::code(operation));
		check_fault(harness, counts, unmapped_name, operation, true, true, 2,
			atomic::data_page + atomic::Page{}.size(), true, true);

		char permission_name[96];
		std::snprintf(permission_name, sizeof(permission_name),
			"operation=%u read-only", pair128::code(operation));
		check_fault(harness, counts, permission_name, operation, false, false,
			2, atomic::data_address, true, false);
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	pair128::ExpectedCounts counts;
	alignment_faults(harness, counts);
	mapping_and_permission_faults(harness, counts);
	pair128::require_counts(harness, counts);
	harness.print_summary("arm64-base-lse128-pair-fault");
}
