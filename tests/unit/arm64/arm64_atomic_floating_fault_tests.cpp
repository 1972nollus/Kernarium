// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_floating_test_support.hpp"

#include <array>
#include <cstdio>

namespace fp_atomic = arm64_atomic_floating_test;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;

namespace {

template<typename Function>
void for_each_variant(Function &&function)
{
	for (unsigned ordering = 0; ordering < 4U; ++ordering)
		function(false, (ordering & 2U) != 0U,
			(ordering & 1U) != 0U);
	for (const bool release : {false, true})
		function(true, false, release);
}

void alignment_faults(
	engine::EngineHarness &harness, fp_atomic::ExpectedCounts &counts)
{
	for (const auto &shape : fp_atomic::widths)
		for (const auto operation : fp_atomic::operations)
			for_each_variant([&](bool store_only, bool acquire,
				bool release) {
				for (std::size_t residue = 1; residue < shape.bytes;
				     ++residue) {
					const auto address = atomic::data_address + residue;
					char name[192];
					std::snprintf(name, sizeof(name),
						"GPR element alignment size=%u opc=%u store=%u A=%u R=%u residue=%zu",
						shape.size_code, fp_atomic::opcode(operation),
						static_cast<unsigned>(store_only),
						static_cast<unsigned>(acquire),
						static_cast<unsigned>(release), residue);
					fp_atomic::check_fault(harness, counts, name,
						shape.size_code, operation, acquire, release,
						store_only, 2, 3, 4, address, address,
						true, true, true);
				}
				for (std::size_t residue = 1; residue < 16U; ++residue) {
					const auto address = atomic::data_address + residue;
					char name[192];
					std::snprintf(name, sizeof(name),
						"SP alignment size=%u opc=%u store=%u A=%u R=%u residue=%zu",
						shape.size_code, fp_atomic::opcode(operation),
						static_cast<unsigned>(store_only),
						static_cast<unsigned>(acquire),
						static_cast<unsigned>(release), residue);
					fp_atomic::check_fault(harness, counts, name,
						shape.size_code, operation, acquire, release,
						store_only, 31, 3, 4, address, address,
						true, true, true);
				}
			});
}

void permission_and_mapping_faults(
	engine::EngineHarness &harness, fp_atomic::ExpectedCounts &counts)
{
	for (const auto &shape : fp_atomic::widths)
		for (const auto operation : fp_atomic::operations)
			for_each_variant([&](bool store_only, bool acquire,
				bool release) {
				for (const unsigned rn : {2U, 31U}) {
					char read_only[192];
					std::snprintf(read_only, sizeof(read_only),
						"read-only size=%u opc=%u store=%u base=%s",
						shape.size_code, fp_atomic::opcode(operation),
						static_cast<unsigned>(store_only),
						rn == 31U ? "sp" : "gpr");
					fp_atomic::check_fault(harness, counts, read_only,
						shape.size_code, operation, acquire, release,
						store_only, rn, 3, 4, atomic::data_address,
						atomic::data_address, true, true, false);

					const auto unmapped =
						atomic::data_page + atomic::Page{}.size();
					char absent[192];
					std::snprintf(absent, sizeof(absent),
						"unmapped size=%u opc=%u store=%u base=%s",
						shape.size_code, fp_atomic::opcode(operation),
						static_cast<unsigned>(store_only),
						rn == 31U ? "sp" : "gpr");
					fp_atomic::check_fault(harness, counts, absent,
						shape.size_code, operation, acquire, release,
						store_only, rn, 3, 4, unmapped, unmapped,
						false, false, false);
				}
				char inaccessible[192];
				std::snprintf(inaccessible, sizeof(inaccessible),
					"no-access size=%u opc=%u store=%u",
					shape.size_code, fp_atomic::opcode(operation),
					static_cast<unsigned>(store_only));
				fp_atomic::check_fault(harness, counts, inaccessible,
					shape.size_code, operation, acquire, release,
					store_only, 2, 3, 4, atomic::data_address,
					atomic::data_address, true, false, false);
			});
}

void page_boundary_successes(
	engine::EngineHarness &harness, fp_atomic::ExpectedCounts &counts)
{
	for (const auto &shape : fp_atomic::widths)
		for (const auto operation : fp_atomic::operations)
			for_each_variant([&](bool store_only, bool acquire,
				bool release) {
				const auto address = atomic::data_page +
					atomic::Page{}.size() - shape.bytes;
				char name[192];
				std::snprintf(name, sizeof(name),
					"last naturally aligned bytes size=%u opc=%u store=%u A=%u R=%u",
					shape.size_code, fp_atomic::opcode(operation),
					static_cast<unsigned>(store_only),
					static_cast<unsigned>(acquire),
					static_cast<unsigned>(release));
				fp_atomic::check_basic(harness, counts, name,
					shape.size_code, operation, acquire, release,
					store_only, 2, 3, 4, address);
			});
}

void allocation_tag_matrix(
	engine::EngineHarness &harness, fp_atomic::ExpectedCounts &counts)
{
	constexpr std::uint8_t logical_tag = 9U;
	constexpr std::uint8_t other_tag = 6U;
	const std::array matching{
		engine::AllocationTag{atomic::data_offset, logical_tag}};
	const std::array mismatching{
		engine::AllocationTag{atomic::data_offset, other_tag}};
	const auto tagged_address =
		(static_cast<std::uint64_t>(logical_tag) << 56) |
		atomic::data_address;

	for (const auto &shape : fp_atomic::widths)
		for (const auto operation : fp_atomic::operations)
			for_each_variant([&](bool store_only, bool acquire,
				bool release) {
				char match_name[192];
				std::snprintf(match_name, sizeof(match_name),
					"matching tag size=%u opc=%u store=%u A=%u R=%u",
					shape.size_code, fp_atomic::opcode(operation),
					static_cast<unsigned>(store_only),
					static_cast<unsigned>(acquire),
					static_cast<unsigned>(release));
				fp_atomic::check(harness, counts, match_name,
					shape.size_code, operation, acquire, release,
					store_only, 2, 3, store_only ? 31U : 4U,
					tagged_address, shape.one, shape.two,
					fp_atomic::basic_result(shape, operation),
					matching, true, false);

				char mismatch_name[192];
				std::snprintf(mismatch_name, sizeof(mismatch_name),
					"mismatching tag size=%u opc=%u store=%u A=%u R=%u",
					shape.size_code, fp_atomic::opcode(operation),
					static_cast<unsigned>(store_only),
					static_cast<unsigned>(acquire),
					static_cast<unsigned>(release));
				fp_atomic::check_fault(harness, counts, mismatch_name,
					shape.size_code, operation, acquire, release,
					store_only, 2, 3, 4, tagged_address,
					atomic::data_address, true, true, true,
					mismatching, true, false);

				char override_name[192];
				std::snprintf(override_name, sizeof(override_name),
					"tag override size=%u opc=%u store=%u A=%u R=%u",
					shape.size_code, fp_atomic::opcode(operation),
					static_cast<unsigned>(store_only),
					static_cast<unsigned>(acquire),
					static_cast<unsigned>(release));
				fp_atomic::check(harness, counts, override_name,
					shape.size_code, operation, acquire, release,
					store_only, 2, 3, store_only ? 31U : 4U,
					tagged_address, shape.one, shape.two,
					fp_atomic::basic_result(shape, operation),
					mismatching, true, true);

				char sp_name[192];
				std::snprintf(sp_name, sizeof(sp_name),
					"SP tag-check disabled size=%u opc=%u store=%u A=%u R=%u",
					shape.size_code, fp_atomic::opcode(operation),
					static_cast<unsigned>(store_only),
					static_cast<unsigned>(acquire),
					static_cast<unsigned>(release));
				fp_atomic::check(harness, counts, sp_name,
					shape.size_code, operation, acquire, release,
					store_only, 31, 3, store_only ? 31U : 4U,
					atomic::data_address, shape.one, shape.two,
					fp_atomic::basic_result(shape, operation),
					mismatching, true, false);
			});
}

void every_logical_tag(
	engine::EngineHarness &harness, fp_atomic::ExpectedCounts &counts)
{
	for (unsigned tag = 0; tag < 16U; ++tag) {
		const std::array tags{
			engine::AllocationTag{atomic::data_offset,
				static_cast<std::uint8_t>(tag)}};
		const auto address = (static_cast<std::uint64_t>(tag) << 56) |
			atomic::data_address;
		for (const auto &shape : fp_atomic::widths)
			for_each_variant([&](bool store_only, bool acquire,
				bool release) {
				char name[160];
				std::snprintf(name, sizeof(name),
					"every logical tag=%u size=%u store=%u A=%u R=%u",
					tag, shape.size_code,
					static_cast<unsigned>(store_only),
					static_cast<unsigned>(acquire),
					static_cast<unsigned>(release));
				fp_atomic::check(harness, counts, name,
					shape.size_code, fp_atomic::Operation::add,
					acquire, release, store_only, 2, 3,
					store_only ? 31U : 4U, address, shape.one,
					shape.two, shape.three, tags, true, false);
			});
	}
}

void alignment_precedes_tag_check(
	engine::EngineHarness &harness, fp_atomic::ExpectedCounts &counts)
{
	const std::array tags{
		engine::AllocationTag{atomic::data_offset, std::uint8_t{3}}};
	const auto tagged_misaligned =
		(std::uint64_t{9} << 56) | (atomic::data_address + 1U);
	for (const auto &shape : fp_atomic::widths)
		for (const auto operation : fp_atomic::operations)
			for_each_variant([&](bool store_only, bool acquire,
				bool release) {
				char name[192];
				std::snprintf(name, sizeof(name),
					"alignment before tag size=%u opc=%u store=%u A=%u R=%u",
					shape.size_code, fp_atomic::opcode(operation),
					static_cast<unsigned>(store_only),
					static_cast<unsigned>(acquire),
					static_cast<unsigned>(release));
				fp_atomic::check_fault(harness, counts, name,
					shape.size_code, operation, acquire, release,
					store_only, 2, 3, 4, tagged_misaligned,
					atomic::data_address + 1U, true, true, true,
					tags, true, false);
			});
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	fp_atomic::ExpectedCounts counts;
	alignment_faults(harness, counts);
	permission_and_mapping_faults(harness, counts);
	page_boundary_successes(harness, counts);
	allocation_tag_matrix(harness, counts);
	every_logical_tag(harness, counts);
	alignment_precedes_tag_check(harness, counts);
	fp_atomic::require(counts.cases == 3864U,
		"fault/tag suite case-count drift");
	fp_atomic::require_counts(harness, counts);
	harness.print_summary("arm64-atomic-floating-fault");
}
