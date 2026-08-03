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

void reserved_allocation_cells(
	engine::EngineHarness &harness, pair128::ExpectedCounts &counts)
{
	const auto initial = atomic::state(0x5100);
	constexpr std::array ordinary_reserved{0U, 2U, 4U, 5U, 6U, 7U};
	constexpr std::array rcw_reserved{4U, 5U, 6U, 7U};
	constexpr std::array software_pair_reserved{0U, 4U, 5U, 6U, 7U};

	for (const bool acquire : {false, true}) {
		for (const bool release : {false, true}) {
			for (const unsigned opcode : ordinary_reserved) {
				char name[128];
				std::snprintf(name, sizeof(name),
					"S=0 o3=0 reserved opc=%u A=%u R=%u", opcode,
					static_cast<unsigned>(acquire),
					static_cast<unsigned>(release));
				pair128::check_illegal(harness, counts, name,
					pair128::raw(false, acquire, release, false, opcode,
						2, 3, 4),
					initial);
			}
			for (const unsigned opcode : rcw_reserved) {
				char name[128];
				std::snprintf(name, sizeof(name),
					"S=0 o3=1 reserved RCW neighbour opc=%u A=%u R=%u",
					opcode, static_cast<unsigned>(acquire),
					static_cast<unsigned>(release));
				pair128::check_illegal(harness, counts, name,
					pair128::raw(false, acquire, release, true, opcode,
						2, 3, 4),
					initial);
			}
			for (unsigned opcode = 0; opcode < 8; ++opcode) {
				char name[128];
				std::snprintf(name, sizeof(name),
					"S=1 o3=0 unallocated opc=%u A=%u R=%u", opcode,
					static_cast<unsigned>(acquire),
					static_cast<unsigned>(release));
				pair128::check_illegal(harness, counts, name,
					pair128::raw(true, acquire, release, false, opcode,
						2, 3, 4),
					initial);
			}
			for (const unsigned opcode : software_pair_reserved) {
				char name[128];
				std::snprintf(name, sizeof(name),
					"S=1 o3=1 reserved opc=%u A=%u R=%u", opcode,
					static_cast<unsigned>(acquire),
					static_cast<unsigned>(release));
				pair128::check_illegal(harness, counts, name,
					pair128::raw(true, acquire, release, true, opcode,
						2, 3, 4),
					initial);
			}
		}
	}
}

void transfer_register_constraints(
	engine::EngineHarness &harness, pair128::ExpectedCounts &counts)
{
	const auto initial = atomic::state(0x5200);
	for (const auto operation : operations) {
		for (const bool acquire : {false, true}) {
			for (const bool release : {false, true}) {
				for (const auto registers : {
				     std::array{31U, 4U},
				     std::array{3U, 31U},
				     std::array{3U, 3U},
				}) {
					char name[128];
					std::snprintf(name, sizeof(name),
						"operation=%u invalid rt=%u rt2=%u A=%u R=%u",
						pair128::code(operation), registers[0], registers[1],
						static_cast<unsigned>(acquire),
						static_cast<unsigned>(release));
					pair128::check_illegal(harness, counts, name,
						pair128::instruction(operation, acquire, release,
							2, registers[0], registers[1]),
						initial);
				}
			}
		}
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	pair128::ExpectedCounts counts;
	reserved_allocation_cells(harness, counts);
	transfer_register_constraints(harness, counts);
	pair128::require_counts(harness, counts);
	harness.print_summary("arm64-base-lse128-pair-allocation");
}
