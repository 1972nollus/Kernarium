// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_two_register_test_support.hpp"

#include <array>

namespace test = arm64_simd_two_register_test;

namespace {

constexpr std::array pairwise_opcodes{2U, 6U};
constexpr test::Registers registers{0U, 1U};

void check_custom(test::engine_tests::EngineHarness &harness,
	const char *name, const test::arm64::CpuState &initial, bool q,
	bool unsigned_group, unsigned size, unsigned opcode,
	test::Registers selected = registers)
{
	harness.check_one(name,
		test::instruction(false, q, unsigned_group, size, opcode,
			selected),
		initial,
		test::expected_state(initial, false, q, unsigned_group, size,
			opcode, selected),
		test::native_requirements());
}

void exhaustive_register_fields(test::engine_tests::EngineHarness &harness)
{
	test::for_each_allocated_form(pairwise_opcodes,
		[&](bool scalar, bool q, bool unsigned_group,
		    unsigned size, unsigned opcode) {
			test::check_form(harness, "simd-two-register-pairwise",
				scalar, q, unsigned_group, size, opcode);
		});
}

void signed_and_unsigned_pairs(test::engine_tests::EngineHarness &harness)
{
	for (const bool q : {false, true})
		for (const bool unsigned_group : {false, true})
			for (unsigned size = 0; size < 3U; ++size)
				for (const auto opcode : pairwise_opcodes) {
					auto initial = test::state(
						opcode * 37U + size * 19U +
						unsigned_group * 11U + q * 5U);
					initial.vectors[registers.source].bytes.fill(
						std::byte{});
					initial.vectors[registers.destination].bytes.fill(
						std::byte{0x5a});
					const std::size_t lane_size =
						std::size_t{1} << size;
					const unsigned lane_bits =
						static_cast<unsigned>(lane_size * 8U);
					const std::size_t active = q ? 16U : 8U;
					for (std::size_t offset = 0; offset < active;
					     offset += lane_size * 2U) {
						const auto first = unsigned_group ?
							test::mask(lane_bits) :
							std::uint64_t{1} <<
								(lane_bits - 1U);
						const auto second = unsigned_group ?
							1U :
							(std::uint64_t{1} <<
								(lane_bits - 1U)) -
								1U;
						test::write_lane(
							initial.vectors[registers.source].bytes,
							offset, lane_size, first);
						test::write_lane(
							initial.vectors[registers.source].bytes,
							offset + lane_size, lane_size, second);
					}
					check_custom(harness,
						unsigned_group ?
							"simd-unsigned-pairwise-wide-sum" :
							"simd-signed-pairwise-minus-one",
						initial, q, unsigned_group, size, opcode);
				}
}

void accumulating_modulo_and_alias(
	test::engine_tests::EngineHarness &harness)
{
	for (const bool q : {false, true})
		for (const bool unsigned_group : {false, true})
			for (unsigned size = 0; size < 3U; ++size) {
				auto initial = test::state(
					size * 43U + unsigned_group * 17U + q * 3U);
				initial.vectors[registers.source].bytes.fill(std::byte{});
				initial.vectors[registers.destination].bytes.fill(
					std::byte{});
				const std::size_t lane_size = std::size_t{1} << size;
				const std::size_t wide_size = lane_size * 2U;
				const unsigned wide_bits =
					static_cast<unsigned>(wide_size * 8U);
				const std::size_t active = q ? 16U : 8U;
				for (std::size_t offset = 0; offset < active;
				     offset += wide_size) {
					test::write_lane(
						initial.vectors[registers.source].bytes,
						offset, lane_size, 1U);
					test::write_lane(
						initial.vectors[registers.destination].bytes,
						offset, wide_size, test::mask(wide_bits));
				}
				check_custom(harness,
					"simd-pairwise-accumulate-modulo",
					initial, q, unsigned_group, size, 6U);

				auto destructive = test::state(
					size * 53U + unsigned_group * 29U + q * 7U);
				check_custom(harness,
					"simd-pairwise-destructive-snapshots-source",
					destructive, q, unsigned_group, size, 6U,
					{5U, 5U});
			}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	exhaustive_register_fields(harness);
	signed_and_unsigned_pairs(harness);
	accumulating_modulo_and_alias(harness);
	harness.print_summary("arm64-simd-two-register-pairwise");
	return 0;
}
