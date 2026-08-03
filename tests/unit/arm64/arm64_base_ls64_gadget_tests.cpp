// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_ls64_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

namespace ls64 = arm64_base_ls64_test;

namespace {

[[nodiscard]] constexpr std::uint16_t register_offset(
	unsigned index) noexcept
{
	static_assert(offsetof(ls64::arm64::CpuState, registers) == 0);
	return static_cast<std::uint16_t>(
		index * sizeof(std::uint64_t));
}

[[nodiscard]] constexpr std::uintptr_t expected_operand(unsigned first,
	unsigned base, unsigned status, bool base_sp,
	bool status_discard) noexcept
{
	return static_cast<std::uintptr_t>(register_offset(first)) |
		(static_cast<std::uintptr_t>(
			base_sp ? 0U : register_offset(base)) << 16) |
		(static_cast<std::uintptr_t>(
			status_discard ? 0U : register_offset(status)) << 32);
}

[[nodiscard]] std::array<std::uintptr_t, 2> build_pair(
	ls64::Operation operation, bool base_sp, bool status_discard)
{
	std::array<std::uintptr_t, 2> entries{};
	for (unsigned variant = 0; variant < 2; ++variant) {
		const unsigned first = variant == 0 ? 0U : 22U;
		const unsigned base = base_sp ? 31U :
			variant == 0 ? 1U : 30U;
		const unsigned status = !ls64::has_status(operation) ||
				status_discard ?
			31U : variant == 0 ? 2U : 17U;
		const auto built = ls64::gadgets::Builder{}.build(
			0x6c64'0000ULL,
			std::array{ls64::encode(
				operation, first, base, status)});
		ls64::require(built.complete);
		ls64::require(built.program.instruction_count() == 1);
		ls64::require(built.program.words().size() == 5);
		entries[variant] = built.program.words()[0];
		ls64::require(built.program.words()[1] == expected_operand(
			first, base, status, base_sp, status_discard));
	}
	return entries;
}

void production_specialization()
{
	std::set<std::uintptr_t> entries;
	for (const auto operation :
	     {ls64::Operation::load, ls64::Operation::store,
	      ls64::Operation::store_status,
	      ls64::Operation::store_status_accdata})
		for (const bool base_sp : {false, true}) {
			const unsigned status_modes =
				ls64::has_status(operation) ? 2U : 1U;
			for (unsigned status_mode = 0;
			     status_mode < status_modes; ++status_mode) {
				const bool status_discard =
					!ls64::has_status(operation) ||
					status_mode != 0;
				const auto pair = build_pair(
					operation, base_sp, status_discard);
				/* Rt, ordinary Rn, and non-discarded Rs are
				 * translation operands, not runtime opcode choices.
				 */
				ls64::require(pair[0] == pair[1]);
				entries.insert(pair[0]);
			}
		}
	/* Operation, SP addressing, and discarded-status choices are all
	 * specialized: 2 + 2 + 4 + 4 exact production entries.
	 */
	ls64::require(entries.size() == 12);
}

} // namespace

int main()
{
	production_specialization();
}
