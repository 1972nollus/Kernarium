// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_rcw_test_support.hpp"

#include <array>

namespace rcw = arm64_base_rcw_test;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;

namespace {

constexpr std::array operations{
	rcw::Operation::compare_swap,
	rcw::Operation::clear,
	rcw::Operation::swap,
	rcw::Operation::set,
};

[[nodiscard]] rcw::arm64::CpuState make_initial(
	rcw::Operation operation, unsigned rn, unsigned source, unsigned result,
	std::uint64_t address, std::uint64_t operand, std::uint64_t compare,
	unsigned seed)
{
	auto initial = atomic::state(seed);
	atomic::write_register(initial, source, operand);
	if (operation == rcw::Operation::compare_swap)
		atomic::write_register(initial, result, compare);
	/* The final write resolves every base/input alias as one architectural
	 * register value. The oracle reads all operands from this final snapshot.
	 */
	atomic::write_register(initial, rn, address, true);
	return initial;
}

void operation_order_zero_and_stack_matrix(engine::EngineHarness &harness)
{
	for (const auto operation : operations) {
		for (const bool soft : {false, true}) {
			for (const bool acquire : {false, true}) {
				for (const bool release : {false, true}) {
					for (const unsigned rn : {2U, 31U}) {
						for (const unsigned source : {3U, 31U}) {
							for (const unsigned result : {4U, 31U}) {
								const auto observed =
									operation == rcw::Operation::compare_swap &&
											result == 31U ?
										0U : rcw::observed_value;
								auto initial = make_initial(operation, rn,
									source, result, atomic::data_address,
									rcw::operand_value, observed,
									rcw::code(operation) * 41U +
										static_cast<unsigned>(soft) * 17U +
										static_cast<unsigned>(acquire) * 7U +
										static_cast<unsigned>(release));
								rcw::check_scalar(harness,
									rcw::scalar_instruction(operation, soft,
										acquire, release, rn, source, result),
									operation, soft, rn, source, result,
									observed, initial,
									"scalar RCW operation/order/ZR/SP matrix");
							}
						}
					}
				}
			}
		}
	}
}

void compare_and_software_check_outcomes(engine::EngineHarness &harness)
{
	for (const bool soft : {false, true}) {
		for (const bool acquire : {false, true}) {
			for (const bool release : {false, true}) {
				auto initial = make_initial(rcw::Operation::compare_swap,
					2, 3, 4, atomic::data_address, rcw::operand_value,
					rcw::observed_value ^ 2U,
					100U + static_cast<unsigned>(soft) * 4U +
						static_cast<unsigned>(acquire) * 2U +
						static_cast<unsigned>(release));
				rcw::check_scalar(harness,
					rcw::scalar_compare_swap(soft, acquire, release,
						2, 4, 3),
					rcw::Operation::compare_swap, soft, 2, 3, 4,
					rcw::observed_value, initial,
					"scalar RCW compare failure");
			}
		}
	}

	for (const auto operation : operations) {
		std::uint64_t observed = rcw::observed_value;
		std::uint64_t operand = rcw::operand_value | 1U;
		if (operation == rcw::Operation::clear) {
			observed |= 1U;
			operand = 1U;
		} else if (operation == rcw::Operation::set) {
			observed &= ~std::uint64_t{1};
			operand = 1U;
		}
		auto initial = make_initial(operation, 2, 3, 4,
			atomic::data_address, operand, observed,
			140U + rcw::code(operation));
		rcw::check_scalar(harness,
			rcw::scalar_instruction(operation, true, true, true,
				2, 3, 4),
			operation, true, 2, 3, 4, observed, initial,
			"scalar RCW software state-check failure");
	}
}

void every_source_result_and_alias(engine::EngineHarness &harness)
{
	constexpr unsigned rn = 29;
	for (const auto operation : operations) {
		for (unsigned source = 0; source < 32; ++source) {
			for (unsigned result = 0; result < 32; ++result) {
				const bool soft =
					((source + result + rcw::code(operation)) & 1U) != 0;
				const bool acquire = (source & 1U) != 0;
				const bool release = (result & 1U) != 0;
				auto initial = make_initial(operation, rn, source, result,
					atomic::data_address,
					rcw::operand_value ^
						(static_cast<std::uint64_t>(source) << 33),
					rcw::observed_value ^
						(static_cast<std::uint64_t>(result) << 34),
					200U + source * 32U + result);
				const auto observed =
					operation == rcw::Operation::compare_swap ?
						atomic::read_register(initial, result) :
						rcw::observed_value ^
							(static_cast<std::uint64_t>(result) << 34);
				rcw::check_scalar(harness,
					rcw::scalar_instruction(operation, soft, acquire,
						release, rn, source, result),
					operation, soft, rn, source, result, observed,
					initial,
					"scalar RCW every source/result/alias");
			}
		}
	}
}

void every_base_register_and_base_alias(engine::EngineHarness &harness)
{
	for (const auto operation : operations) {
		for (unsigned rn = 0; rn < 32; ++rn) {
			const unsigned source = 3;
			const unsigned result = 4;
			auto initial = make_initial(operation, rn, source, result,
				atomic::data_address, rcw::operand_value,
				rcw::observed_value, 4400U + rn +
					rcw::code(operation) * 32U);
			const auto observed =
				operation == rcw::Operation::compare_swap ?
					atomic::read_register(initial, result) :
					rcw::observed_value;
			rcw::check_scalar(harness,
				rcw::scalar_instruction(operation, (rn & 1U) != 0,
					(rn & 2U) != 0, (rn & 4U) != 0, rn, source,
					result),
				operation, (rn & 1U) != 0, rn, source, result,
				observed, initial,
				"scalar RCW every base register");
		}
	}
}

void aligned_page_positions(engine::EngineHarness &harness)
{
	const std::array addresses{
		atomic::data_page,
		atomic::data_address,
		atomic::data_page + DARWIN_UML_GUEST_PAGE_SIZE - 8U,
		atomic::data_page + DARWIN_UML_GUEST_PAGE_SIZE,
		atomic::data_page + 2U * DARWIN_UML_GUEST_PAGE_SIZE - 8U,
	};
	for (const auto operation : operations) {
		for (unsigned index = 0; index < addresses.size(); ++index) {
			auto initial = make_initial(operation, 2, 3, 4,
				addresses[index], rcw::operand_value,
				rcw::observed_value,
				5000U + rcw::code(operation) * 8U + index);
			rcw::check_scalar(harness,
				rcw::scalar_instruction(operation, false,
					(index & 1U) != 0, (index & 2U) != 0,
					2, 3, 4),
				operation, false, 2, 3, 4, rcw::observed_value,
				initial, "scalar RCW aligned page position");
		}
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	operation_order_zero_and_stack_matrix(harness);
	compare_and_software_check_outcomes(harness);
	every_source_result_and_alias(harness);
	every_base_register_and_base_alias(harness);
	aligned_page_positions(harness);
	harness.print_summary("arm64-base-rcw-scalar");
}
