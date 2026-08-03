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
constexpr rcw::Pair observed{
	rcw::observed_value,
	rcw::observed_value2,
};
constexpr rcw::Pair operand{
	rcw::operand_value,
	rcw::operand_value2,
};

[[nodiscard]] rcw::arm64::CpuState make_initial(
	rcw::Operation operation, unsigned rn, unsigned first, unsigned second,
	std::uint64_t address, const rcw::Pair &source,
	const rcw::Pair &compare, unsigned seed)
{
	auto initial = atomic::state(seed);
	if (operation == rcw::Operation::compare_swap) {
		rcw::write_pair(initial, first, source);
		rcw::write_pair(initial, second, compare);
	} else {
		atomic::write_register(initial, first, source[0]);
		atomic::write_register(initial, second, source[1]);
	}
	/* Resolve base/transfer aliases before the test-only oracle snapshots any
	 * compare or replacement value.
	 */
	atomic::write_register(initial, rn, address, true);
	return initial;
}

void operation_order_pair_zero_and_stack_matrix(
	engine::EngineHarness &harness)
{
	constexpr std::array compare_swap_modes{
		std::array{2U, 4U},
		std::array{30U, 4U},
		std::array{2U, 30U},
		std::array{30U, 30U},
		std::array{4U, 4U},
	};
	constexpr std::array modify_modes{
		std::array{3U, 4U},
		std::array{4U, 3U},
		std::array{0U, 30U},
		std::array{7U, 8U},
	};
	for (const auto operation : operations) {
		for (const bool soft : {false, true}) {
			for (const bool acquire : {false, true}) {
				for (const bool release : {false, true}) {
					for (const unsigned rn : {6U, 31U}) {
						if (operation == rcw::Operation::compare_swap) {
							for (const auto mode : compare_swap_modes) {
								auto initial = make_initial(operation, rn,
									mode[0], mode[1], atomic::data_address,
									operand, observed,
									100U + static_cast<unsigned>(soft) * 32U +
										static_cast<unsigned>(acquire) * 16U +
										static_cast<unsigned>(release) * 8U +
										mode[0]);
								const auto memory_value =
									rcw::read_pair(initial, mode[1]);
								rcw::check_pair(harness,
									rcw::pair_instruction(operation, soft,
										acquire, release, rn, mode[0],
										mode[1]),
									operation, soft, rn, mode[0], mode[1],
									memory_value, initial,
									"pair RCW operation/order/ZR/SP matrix");
							}
						} else {
							for (const auto mode : modify_modes) {
								auto initial = make_initial(operation, rn,
									mode[0], mode[1], atomic::data_address,
									operand, {}, 300U +
										rcw::code(operation) * 64U +
										static_cast<unsigned>(soft) * 16U +
										mode[0]);
								rcw::check_pair(harness,
									rcw::pair_instruction(operation, soft,
										acquire, release, rn, mode[0],
										mode[1]),
									operation, soft, rn, mode[0], mode[1],
									observed, initial,
									"pair RCW operation/order/SP matrix");
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
		for (unsigned mismatch = 0; mismatch < 2; ++mismatch) {
			auto compare = observed;
			compare[mismatch] ^= 2U;
			auto initial = make_initial(rcw::Operation::compare_swap,
				6, 2, 4, atomic::data_address, operand, compare,
				600U + static_cast<unsigned>(soft) * 2U + mismatch);
			rcw::check_pair(harness,
				rcw::pair_compare_swap(soft, true, true, 6, 4, 2),
				rcw::Operation::compare_swap, soft, 6, 2, 4,
				observed, initial, "pair RCW compare failure");
		}
	}

	for (const auto operation : operations) {
		auto old = observed;
		auto source = operand;
		if (operation == rcw::Operation::clear) {
			old[0] |= 1U;
			source = {1U, 0U};
		} else {
			old[0] &= ~std::uint64_t{1};
			source[0] |= 1U;
		}
		auto initial = make_initial(operation, 6, 2, 4,
			atomic::data_address, source, old,
			640U + rcw::code(operation));
		rcw::check_pair(harness,
			rcw::pair_instruction(operation, true, true, true,
				6, 2, 4),
			operation, true, 6, 2, 4, old, initial,
			"pair RCW software state-check failure");

		/* Changing only the high doubleword leaves the descriptor state bit
		 * unchanged and must pass the software check.
		 */
		old = observed;
		source = operation == rcw::Operation::clear ?
			rcw::Pair{0U, 1U} : old;
		if (operation != rcw::Operation::clear)
			source[1] ^= 1U;
		initial = make_initial(operation, 6, 2, 4,
			atomic::data_address, source, old,
			660U + rcw::code(operation));
		rcw::check_pair(harness,
			rcw::pair_instruction(operation, true, false, false,
				6, 2, 4),
			operation, true, 6, 2, 4, old, initial,
			"pair RCW high-doubleword software check");
	}
}

void every_even_compare_and_replacement_pair(
	engine::EngineHarness &harness)
{
	for (unsigned replacement = 0; replacement <= 30; replacement += 2) {
		for (unsigned compare = 0; compare <= 30; compare += 2) {
			const bool soft = ((replacement + compare) & 2U) != 0;
			const bool acquire = (replacement & 2U) != 0;
			const bool release = (compare & 2U) != 0;
			auto initial = make_initial(rcw::Operation::compare_swap,
				6, replacement, compare, atomic::data_address,
				operand, observed,
				700U + replacement * 32U + compare);
			const auto memory_value = rcw::read_pair(initial, compare);
			rcw::check_pair(harness,
				rcw::pair_compare_swap(soft, acquire, release,
					6, compare, replacement),
				rcw::Operation::compare_swap, soft, 6, replacement,
				compare, memory_value, initial,
				"pair RCW every even register pair");
		}
	}
}

void every_distinct_modify_register_pair(engine::EngineHarness &harness)
{
	for (const auto operation : {
	     rcw::Operation::clear,
	     rcw::Operation::swap,
	     rcw::Operation::set,
	}) {
		for (unsigned first = 0; first < 31; ++first) {
			for (unsigned second = 0; second < 31; ++second) {
				if (first == second)
					continue;
				const bool soft =
					((first + second + rcw::code(operation)) & 1U) != 0;
				const bool acquire = (first & 1U) != 0;
				const bool release = (second & 1U) != 0;
				const rcw::Pair source{
					rcw::operand_value ^
						(static_cast<std::uint64_t>(first) << 40),
					rcw::operand_value2 ^
						(static_cast<std::uint64_t>(second) << 36),
				};
				auto initial = make_initial(operation, 31, first, second,
					atomic::data_address, source, {},
					1400U + rcw::code(operation) * 1024U +
						first * 32U + second);
				rcw::check_pair(harness,
					rcw::pair_modify_raw(soft, acquire, release,
						rcw::code(operation), 31, first, second),
					operation, soft, 31, first, second, observed,
					initial,
					"pair RCW every distinct transfer pair");
			}
		}
	}
}

void every_base_register_and_alias(engine::EngineHarness &harness)
{
	for (const auto operation : operations) {
		for (unsigned rn = 0; rn < 32; ++rn) {
			const unsigned first = operation == rcw::Operation::compare_swap ?
				2U : 3U;
			const unsigned second = 4U;
			auto initial = make_initial(operation, rn, first, second,
				atomic::data_address, operand, observed,
				4800U + rcw::code(operation) * 32U + rn);
			const auto memory_value =
				operation == rcw::Operation::compare_swap ?
					rcw::read_pair(initial, second) : observed;
			const bool soft = (rn & 1U) != 0;
			rcw::check_pair(harness,
				rcw::pair_instruction(operation, soft,
					(rn & 2U) != 0, (rn & 4U) != 0, rn, first,
					second),
				operation, soft, rn, first, second, memory_value,
				initial, "pair RCW every base register");
		}
	}
}

void aligned_page_positions(engine::EngineHarness &harness)
{
	const std::array addresses{
		atomic::data_page,
		atomic::data_address,
		atomic::data_page + DARWIN_UML_GUEST_PAGE_SIZE - 16U,
		atomic::data_page + DARWIN_UML_GUEST_PAGE_SIZE,
		atomic::data_page + 2U * DARWIN_UML_GUEST_PAGE_SIZE - 16U,
	};
	for (const auto operation : operations) {
		for (unsigned index = 0; index < addresses.size(); ++index) {
			auto initial = make_initial(operation, 6, 2, 4,
				addresses[index], operand, observed,
				5000U + rcw::code(operation) * 8U + index);
			const auto memory_value =
				operation == rcw::Operation::compare_swap ?
					rcw::read_pair(initial, 4) : observed;
			rcw::check_pair(harness,
				rcw::pair_instruction(operation, false,
					(index & 1U) != 0, (index & 2U) != 0,
					6, 2, 4),
				operation, false, 6, 2, 4, memory_value, initial,
				"pair RCW aligned page position");
		}
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	operation_order_pair_zero_and_stack_matrix(harness);
	compare_and_software_check_outcomes(harness);
	every_even_compare_and_replacement_pair(harness);
	every_distinct_modify_register_pair(harness);
	every_base_register_and_alias(harness);
	aligned_page_positions(harness);
	harness.print_summary("arm64-base-rcw-pair");
}
