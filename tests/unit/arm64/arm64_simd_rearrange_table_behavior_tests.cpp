// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_rearrange_test_support.hpp"
#include "arm64_simd_test_support.hpp"

#include <array>
#include <cstddef>

namespace test = arm64_simd_rearrange_test;
namespace base = arm64_simd_test;

namespace {

constexpr std::array boundary_patterns{
	std::array<unsigned, 8>{0U, 15U, 16U, 31U, 32U, 47U, 48U, 255U},
	std::array<unsigned, 8>{14U, 15U, 16U, 17U, 30U, 31U, 32U, 33U},
	std::array<unsigned, 8>{46U, 47U, 48U, 49U, 62U, 63U, 64U, 65U},
};

void set_boundary_indices(uml::cpu::arm64::CpuState &initial,
	unsigned indices, unsigned count, unsigned pattern)
{
	const auto limit = count * 16U;
	const auto &first = boundary_patterns[pattern % boundary_patterns.size()];
	for (unsigned lane = 0; lane < first.size(); ++lane)
		initial.vectors[indices].bytes[lane] =
			static_cast<std::byte>(first[lane]);
	const std::array<unsigned, 8> second{
		0U, limit - 2U, limit - 1U, limit,
		limit + 1U, 63U, 64U, 255U,
	};
	for (unsigned lane = 0; lane < second.size(); ++lane)
		initial.vectors[indices].bytes[lane + 8U] =
			static_cast<std::byte>(second[lane]);
}

void check(test::engine_tests::EngineHarness &harness, bool q, bool extend,
	unsigned count, test::TableRegisters registers, unsigned pattern,
	unsigned seed)
{
	auto initial = test::state(seed);
	set_boundary_indices(initial, registers.indices, count, pattern);
	harness.check_one(test::table_name(extend),
		test::table_instruction(q, extend, count, registers), initial,
		test::expected_table_state(initial, q, extend, count, registers),
		test::native_requirements());
}

void every_width_operation_count_register_field_alias_and_boundary()
{
	test::engine_tests::EngineHarness harness;
	unsigned seed = 0;
	for (const bool q : {false, true})
		for (const bool extend : {false, true})
			for (unsigned count = 1; count <= 4; ++count) {
				/* Exhaust Rd, Rn, and Rm independently. Cycling the
				 * boundary vectors covers every 16-byte table edge,
				 * the final valid index, and out-of-range indices.
				 */
				for (unsigned reg = 0; reg < 32U; ++reg) {
					check(harness, q, extend, count,
						{reg, 5U, 21U}, reg, ++seed);
					check(harness, q, extend, count,
						{9U, reg, 21U}, reg + 1U, ++seed);
					check(harness, q, extend, count,
						{9U, 5U, reg}, reg + 2U, ++seed);
				}

				/* Lists starting at 29--31 exercise architectural
				 * modulo-32 table-register wrapping. Each possible
				 * table member aliases both Rd and Rm in separate
				 * read-before-write cases.
				 */
				for (unsigned first : {29U, 30U, 31U}) {
					check(harness, q, extend, count,
						{7U, first, 23U}, 0U, ++seed);
					check(harness, q, extend, count,
						{7U, first, 7U}, 1U, ++seed);
					check(harness, q, extend, count,
						{first, first, first}, 2U, ++seed);
					for (unsigned member = 0; member < count;
					     ++member) {
						const unsigned table_register =
							(first + member) & 31U;
						check(harness, q, extend, count,
							{table_register, first, 23U},
							member, ++seed);
						check(harness, q, extend, count,
							{7U, first, table_register},
							member + 1U, ++seed);
					}
				}
			}

	constexpr std::size_t field_cases = 2U * 2U * 4U * 32U * 3U;
	constexpr std::size_t alias_cases =
		2U * 2U * 3U * (4U * 3U + 2U * (1U + 2U + 3U + 4U));
	constexpr std::size_t cases = field_cases + alias_cases;
	static_assert(cases == 1920U);
	base::require(harness.counts().interpreter == cases);
	base::require(harness.counts().gadgets == cases);
	base::require(
		harness.counts().jit + harness.counts().jit_skipped == cases);
	harness.print_summary("arm64-simd-rearrange-table");
}

} // namespace

int main()
{
	every_width_operation_count_register_field_alias_and_boundary();
	return 0;
}
