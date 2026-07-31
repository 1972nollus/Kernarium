// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_floating_test_support.hpp"

#include <cstdio>

namespace fp_atomic = arm64_atomic_floating_test;
namespace engine = uml::backend::tests;

namespace {

void every_named_variant(
	engine::EngineHarness &harness, fp_atomic::ExpectedCounts &counts)
{
	unsigned ordinal = 0;
	for (const auto &shape : fp_atomic::widths)
		for (const auto operation : fp_atomic::operations) {
			for (unsigned ordering = 0; ordering < 4U; ++ordering) {
				const bool acquire = (ordering & 2U) != 0U;
				const bool release = (ordering & 1U) != 0U;
				const unsigned rn = ordinal % 32U;
				const unsigned rs = (ordinal * 7U + 3U) % 32U;
				const unsigned rt = (ordinal * 13U + 5U) % 32U;
				char name[160];
				std::snprintf(name, sizeof(name),
					"all load variants size=%u opc=%u A=%u R=%u",
					shape.size_code, fp_atomic::opcode(operation),
					static_cast<unsigned>(acquire),
					static_cast<unsigned>(release));
				fp_atomic::check_basic(harness, counts, name,
					shape.size_code, operation, acquire, release, false,
					rn, rs, rt);
				++ordinal;
			}
			for (const bool release : {false, true}) {
				const unsigned rn = ordinal % 32U;
				const unsigned rs = (ordinal * 11U + 1U) % 32U;
				char name[160];
				std::snprintf(name, sizeof(name),
					"all store variants size=%u opc=%u R=%u",
					shape.size_code, fp_atomic::opcode(operation),
					static_cast<unsigned>(release));
				fp_atomic::check_basic(harness, counts, name,
					shape.size_code, operation, false, release, true,
					rn, rs, 31);
				++ordinal;
			}
		}
}

void every_base_register(
	engine::EngineHarness &harness, fp_atomic::ExpectedCounts &counts)
{
	for (const auto &shape : fp_atomic::widths)
		for (const auto operation : fp_atomic::operations)
			for (unsigned rn = 0; rn < 32U; ++rn) {
				char load_name[160];
				std::snprintf(load_name, sizeof(load_name),
					"every load base size=%u opc=%u rn=%u",
					shape.size_code, fp_atomic::opcode(operation), rn);
				fp_atomic::check_basic(harness, counts, load_name,
					shape.size_code, operation, (rn & 2U) != 0U,
					(rn & 1U) != 0U, false, rn, 3, 4);

				char store_name[160];
				std::snprintf(store_name, sizeof(store_name),
					"every store base size=%u opc=%u rn=%u",
					shape.size_code, fp_atomic::opcode(operation), rn);
				fp_atomic::check_basic(harness, counts, store_name,
					shape.size_code, operation, false,
					(rn & 1U) != 0U, true, rn, 3, 31);
			}
}

void every_source_register(
	engine::EngineHarness &harness, fp_atomic::ExpectedCounts &counts)
{
	for (const auto &shape : fp_atomic::widths)
		for (const auto operation : fp_atomic::operations)
			for (unsigned rs = 0; rs < 32U; ++rs) {
				char load_name[160];
				std::snprintf(load_name, sizeof(load_name),
					"every load source size=%u opc=%u rs=%u",
					shape.size_code, fp_atomic::opcode(operation), rs);
				fp_atomic::check_basic(harness, counts, load_name,
					shape.size_code, operation, (rs & 2U) != 0U,
					(rs & 1U) != 0U, false, 2, rs,
					rs == 4U ? 5U : 4U);

				char store_name[160];
				std::snprintf(store_name, sizeof(store_name),
					"every store source size=%u opc=%u rs=%u",
					shape.size_code, fp_atomic::opcode(operation), rs);
				fp_atomic::check_basic(harness, counts, store_name,
					shape.size_code, operation, false,
					(rs & 1U) != 0U, true, 2, rs, 31);
			}
}

void every_destination_and_alias(
	engine::EngineHarness &harness, fp_atomic::ExpectedCounts &counts)
{
	for (const auto &shape : fp_atomic::widths)
		for (const auto operation : fp_atomic::operations) {
			for (unsigned rt = 0; rt < 32U; ++rt) {
				char name[160];
				std::snprintf(name, sizeof(name),
					"every load destination size=%u opc=%u rt=%u",
					shape.size_code, fp_atomic::opcode(operation), rt);
				fp_atomic::check_basic(harness, counts, name,
					shape.size_code, operation, (rt & 2U) != 0U,
					(rt & 1U) != 0U, false, 2,
					rt == 3U ? 5U : 3U, rt);
			}
			for (unsigned ordering = 0; ordering < 4U; ++ordering)
				for (unsigned reg = 0; reg < 32U; ++reg) {
					char name[160];
					std::snprintf(name, sizeof(name),
						"Rs==Rt alias size=%u opc=%u ordering=%u v=%u",
						shape.size_code, fp_atomic::opcode(operation),
						ordering, reg);
					fp_atomic::check_basic(harness, counts, name,
						shape.size_code, operation,
						(ordering & 2U) != 0U,
						(ordering & 1U) != 0U, false, 2,
						reg, reg);
				}
		}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	fp_atomic::ExpectedCounts counts;
	every_named_variant(harness, counts);
	every_base_register(harness, counts);
	every_source_register(harness, counts);
	every_destination_and_alias(harness, counts);
	fp_atomic::require(counts.cases == 5880U,
		"ordering/alias suite case-count drift");
	fp_atomic::require_counts(harness, counts);
	harness.print_summary("arm64-atomic-floating-ordering-alias");
}
