// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_floating_test_support.hpp"

#include <cstdio>

namespace fp_atomic = arm64_atomic_floating_test;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;

namespace {

void allocated_operation_matrix(
	engine::EngineHarness &harness, fp_atomic::ExpectedCounts &counts)
{
	for (const auto &shape : fp_atomic::widths)
		for (const auto operation : fp_atomic::operations) {
			for (const bool acquire : {false, true})
				for (const bool release : {false, true}) {
					char name[160];
					std::snprintf(name, sizeof(name),
						"%.*s load operation=%u A=%u R=%u",
						static_cast<int>(shape.name.size()), shape.name.data(),
						fp_atomic::opcode(operation),
						static_cast<unsigned>(acquire),
						static_cast<unsigned>(release));
					fp_atomic::check_basic(harness, counts, name,
						shape.size_code, operation, acquire, release,
						false);
				}
			for (const bool release : {false, true}) {
				char name[160];
				std::snprintf(name, sizeof(name),
					"%.*s store operation=%u R=%u",
					static_cast<int>(shape.name.size()), shape.name.data(),
					fp_atomic::opcode(operation),
					static_cast<unsigned>(release));
				fp_atomic::check_basic(harness, counts, name,
					shape.size_code, operation, false, release, true);
			}
		}
}

void reserved_opcode_cells(
	engine::EngineHarness &harness, fp_atomic::ExpectedCounts &counts)
{
	auto initial = atomic::state(0x2200);
	initial.registers[2] = atomic::data_page + atomic::Page{}.size() + 1U;
	initial.sp = initial.registers[2];
	initial.mte_enabled = true;
	for (unsigned size = 0; size < 4U; ++size)
		for (const bool store_only : {false, true})
			for (const unsigned opcode : {1U, 2U, 3U})
				for (const bool acquire : {false, true})
					for (const bool release : {false, true}) {
						char name[160];
						std::snprintf(name, sizeof(name),
							"reserved opc=%u size=%u o3=%u A=%u R=%u",
							opcode, size,
							static_cast<unsigned>(store_only),
							static_cast<unsigned>(acquire),
							static_cast<unsigned>(release));
						fp_atomic::check_illegal(harness, counts, name,
							fp_atomic::raw(size, acquire, release,
								store_only, opcode, 2, 3,
								store_only ? 31U : 4U),
							initial);
					}
}

void malformed_store_only_cells(
	engine::EngineHarness &harness, fp_atomic::ExpectedCounts &counts)
{
	auto initial = atomic::state(0x2300);
	initial.registers[2] = atomic::data_page + atomic::Page{}.size() + 1U;
	initial.sp = initial.registers[2];
	initial.mte_enabled = true;
	for (unsigned size = 0; size < 4U; ++size)
		for (const auto operation : fp_atomic::operations)
			for (const bool release : {false, true}) {
				/* A must be zero for store-only encodings, for every Rt. */
				for (unsigned rt = 0; rt < 32U; ++rt) {
					char name[160];
					std::snprintf(name, sizeof(name),
						"store-only acquire reserved size=%u opc=%u R=%u rt=%u",
						size, fp_atomic::opcode(operation),
						static_cast<unsigned>(release), rt);
					fp_atomic::check_illegal(harness, counts, name,
						fp_atomic::raw(size, true, release, true,
							fp_atomic::opcode(operation), 2, 3, rt),
						initial);
				}
				/* With A clear, only Rt==31 is allocated. */
				for (unsigned rt = 0; rt < 31U; ++rt) {
					char name[160];
					std::snprintf(name, sizeof(name),
						"store-only non-31 Rt reserved size=%u opc=%u R=%u rt=%u",
						size, fp_atomic::opcode(operation),
						static_cast<unsigned>(release), rt);
					fp_atomic::check_illegal(harness, counts, name,
						fp_atomic::raw(size, false, release, true,
							fp_atomic::opcode(operation), 2, 3, rt),
						initial);
				}
			}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	fp_atomic::ExpectedCounts counts;
	allocated_operation_matrix(harness, counts);
	reserved_opcode_cells(harness, counts);
	malformed_store_only_cells(harness, counts);
	fp_atomic::require(counts.cases == 2736U,
		"allocation suite case-count drift");
	fp_atomic::require_counts(harness, counts);
	harness.print_summary("arm64-atomic-floating-allocation");
}
