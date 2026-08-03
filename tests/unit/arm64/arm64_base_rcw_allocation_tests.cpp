// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_rcw_test_support.hpp"

namespace rcw = arm64_base_rcw_test;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;

namespace {

void canonical_words_match_specification()
{
	rcw::require(
		rcw::scalar_compare_swap(false, false, false, 2, 4, 3) ==
			0x1924'0843U,
		"scalar compare-and-swap canonical encoding");
	rcw::require(
		rcw::scalar_modify_raw(true, true, true, 3, 2, 3, 4) ==
			0x78e3'b044U,
		"scalar modify canonical encoding");
	rcw::require(
		rcw::pair_compare_swap(true, true, true, 6, 2, 4) ==
			0x59e2'0cc4U,
		"pair compare-and-swap canonical encoding");
	rcw::require(
		rcw::pair_modify_raw(true, true, true, 1, 6, 3, 4) ==
			0x59e4'90c3U,
		"pair modify canonical encoding");
}

void scalar_reserved_operation_cells(engine::EngineHarness &harness)
{
	std::size_t invalid = 0;
	std::size_t adjacent_ordered_loads = 0;
	for (unsigned operation = 4; operation < 8; ++operation) {
		for (const bool soft : {false, true}) {
			for (const bool acquire : {false, true}) {
				for (const bool release : {false, true}) {
					for (unsigned selector = 0; selector < 32; ++selector) {
						const unsigned rn = selector;
						const unsigned source = (selector + 11U) & 31U;
						const unsigned result = (selector + 23U) & 31U;
						/* o3=1/opcode=100 with A=1,R=0,Rs=31 is the
						 * neighboring ordered-load allocation, for both
						 * encodable transfer sizes. It is deliberately
						 * excluded instead of being misclassified as RCW.
						 */
						if (operation == 4U && acquire && !release &&
						    source == 31U) {
							++adjacent_ordered_loads;
							continue;
						}
						auto initial = atomic::state(
							operation * 1024U + selector * 8U +
							static_cast<unsigned>(soft) * 4U +
							static_cast<unsigned>(acquire) * 2U +
							static_cast<unsigned>(release));
						harness.check_illegal(
							"scalar RCW reserved operation cell",
							rcw::scalar_modify_raw(soft, acquire, release,
								operation, rn, source, result),
							initial, rcw::scalar_native());
						++invalid;
					}
				}
			}
		}
	}
	rcw::require(invalid == 1022U,
		"scalar reserved-operation partition count");
	rcw::require(adjacent_ordered_loads == 2U,
		"scalar neighboring ordered-load partition count");
}

void pair_compare_register_constraints(engine::EngineHarness &harness)
{
	std::size_t invalid = 0;
	for (const bool soft : {false, true}) {
		for (const bool acquire : {false, true}) {
			for (const bool release : {false, true}) {
				for (unsigned replacement = 0; replacement < 32;
				     ++replacement) {
					for (unsigned compare = 0; compare < 32; ++compare) {
						if ((replacement & 1U) == 0 &&
						    (compare & 1U) == 0)
							continue;
						auto initial = atomic::state(
							replacement * 32U + compare);
						harness.check_illegal(
							"pair RCW odd pair-start encoding",
							rcw::pair_compare_swap(soft, acquire, release,
								6, compare, replacement),
							initial, rcw::pair_native());
						++invalid;
					}
				}
			}
		}
	}
	rcw::require(invalid == 6144U,
		"pair compare-and-swap invalid partition count");
}

void pair_modify_register_constraints(engine::EngineHarness &harness)
{
	std::size_t invalid = 0;
	for (unsigned operation = 1; operation <= 3; ++operation) {
		for (const bool soft : {false, true}) {
			for (const bool acquire : {false, true}) {
				for (const bool release : {false, true}) {
					for (unsigned first = 0; first < 32; ++first) {
						for (unsigned second = 0; second < 32; ++second) {
							if (first != 31U && second != 31U &&
							    first != second)
								continue;
							/*
							 * Equal non-ZR transfers are constrained
							 * unpredictable. This implementation selects the
							 * architecturally permitted undefined behavior;
							 * XZR fields are unconditionally unallocated.
							 */
							auto initial = atomic::state(
								operation * 2048U + first * 32U + second);
							harness.check_illegal(
								"pair RCW invalid or undefined transfer tuple",
								rcw::pair_modify_raw(soft, acquire, release,
									operation, 6, first, second),
								initial, rcw::pair_native());
							++invalid;
						}
					}
				}
			}
		}
	}
	rcw::require(invalid == 2256U,
		"pair modify invalid partition count");
}

void pair_reserved_operation_cells(engine::EngineHarness &harness)
{
	std::size_t invalid = 0;
	/* S=1,o3=1,opc=000 is unallocated. Its S=0 counterpart is the
	 * neighboring SWPP family and is intentionally not claimed here. */
	for (const bool acquire : {false, true}) {
		for (const bool release : {false, true}) {
			for (unsigned selector = 0; selector < 32; ++selector) {
				auto initial = atomic::state(selector * 4U +
					static_cast<unsigned>(acquire) * 2U +
					static_cast<unsigned>(release));
				harness.check_illegal(
					"pair software RCW reserved operation-zero cell",
					rcw::pair_modify_raw(true, acquire, release, 0,
						selector, (selector + 7U) % 31U,
						(selector + 19U) % 31U),
					initial, rcw::pair_native());
				++invalid;
			}
		}
	}
	for (unsigned operation = 4; operation < 8; ++operation) {
		for (const bool soft : {false, true}) {
			for (const bool acquire : {false, true}) {
				for (const bool release : {false, true}) {
					for (unsigned selector = 0; selector < 32; ++selector) {
						auto initial =
							atomic::state(operation * 1024U + selector);
						harness.check_illegal(
							"pair RCW reserved operation cell",
							rcw::pair_modify_raw(soft, acquire, release,
								operation, selector,
								(selector + 7U) % 31U,
								(selector + 19U) % 31U),
							initial, rcw::pair_native());
						++invalid;
					}
				}
			}
		}
	}
	rcw::require(invalid == 1152U,
		"pair reserved-operation partition count");
}

} // namespace

int main()
{
	canonical_words_match_specification();
	engine::EngineHarness harness;
	scalar_reserved_operation_cells(harness);
	pair_compare_register_constraints(harness);
	pair_modify_register_constraints(harness);
	pair_reserved_operation_cells(harness);
	harness.print_summary("arm64-base-rcw-allocation");
}
