// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_two_register_test_support.hpp"

#include <array>

namespace test = arm64_simd_two_register_test;

namespace {

constexpr std::array narrow_and_shift_opcodes{18U, 19U, 20U};
constexpr test::Registers registers{0U, 1U};

void check_custom(test::engine_tests::EngineHarness &harness,
	const char *name, const test::arm64::CpuState &initial,
	bool scalar, bool q, bool unsigned_group, unsigned size, unsigned opcode,
	test::Registers selected = registers)
{
	harness.check_one(name,
		test::instruction(scalar, q, unsigned_group, size, opcode,
			selected),
		initial,
		test::expected_state(initial, scalar, q, unsigned_group, size,
			opcode, selected),
		test::native_requirements());
}

void exhaustive_register_fields(test::engine_tests::EngineHarness &harness)
{
	test::for_each_allocated_form(narrow_and_shift_opcodes,
		[&](bool scalar, bool q, bool unsigned_group,
		    unsigned size, unsigned opcode) {
			test::check_form(harness, "simd-two-register-narrow-shift",
				scalar, q, unsigned_group, size, opcode);
		});
}

void narrow_boundaries(test::engine_tests::EngineHarness &harness)
{
	constexpr std::array narrow_opcodes{18U, 20U};
	test::for_each_allocated_form(narrow_opcodes,
		[&](bool scalar, bool q, bool unsigned_group,
		    unsigned size, unsigned opcode) {
			auto initial = test::state(
				opcode * 47U + size * 19U +
				unsigned_group * 13U + scalar * 7U + q * 3U);
			initial.vectors[registers.source].bytes.fill(std::byte{});
			const std::size_t destination_size =
				std::size_t{1} << size;
			const unsigned destination_bits =
				static_cast<unsigned>(destination_size * 8U);
			const std::size_t source_size = destination_size * 2U;
			const unsigned source_bits = destination_bits * 2U;
			const std::size_t source_active =
				scalar ? source_size : 16U;
			for (std::size_t offset = 0, lane = 0;
			     offset < source_active;
			     offset += source_size, ++lane) {
				std::uint64_t value;
				if (opcode == 18U && !unsigned_group) {
					value = (std::uint64_t{0xa5} <<
						 destination_bits) |
						(lane * 37U + 0x5aU);
				} else if (opcode == 18U) {
					value = lane & 1U ?
						(std::uint64_t{1} << destination_bits) :
						test::mask(source_bits);
				} else if (unsigned_group) {
					value = (std::uint64_t{1}
						<< destination_bits) + lane;
				} else {
					const __int128 below =
						-(static_cast<__int128>(1)
							<< (destination_bits - 1U)) -
						1;
					const __int128 above =
						(static_cast<__int128>(1)
							<< (destination_bits - 1U));
					value = static_cast<std::uint64_t>(
						static_cast<unsigned __int128>(
							lane & 1U ? above : below)) &
						test::mask(source_bits);
				}
				test::write_lane(
					initial.vectors[registers.source].bytes,
					offset, source_size, value);
			}
			check_custom(harness,
				opcode == 18U && !unsigned_group ?
					"simd-truncating-narrow-boundary" :
					"simd-saturating-narrow-boundary",
				initial, scalar, q, unsigned_group, size, opcode);

			if (opcode == 18U && !unsigned_group)
				return;
			initial.vectors[registers.source].bytes.fill(std::byte{});
			initial.fpsr |= 1U << 27;
			check_custom(harness,
				"simd-saturating-narrow-qc-sticky",
				initial, scalar, q, unsigned_group, size, opcode);
		});
}

void upper_merge_destructive(test::engine_tests::EngineHarness &harness)
{
	constexpr std::array narrow_opcodes{18U, 20U};
	for (const auto opcode : narrow_opcodes)
		for (const bool unsigned_group : {false, true})
			for (unsigned size = 0; size < 3U; ++size) {
				if (!test::allocated(false, true, unsigned_group,
				    size, opcode))
					continue;
				auto initial = test::state(
					opcode * 59U + unsigned_group * 23U +
					size * 5U);
				check_custom(harness,
					"simd-upper-narrow-destructive-snapshot",
					initial, false, true, unsigned_group,
					size, opcode, {9U, 9U});
			}
}

void widening_shift_halves(test::engine_tests::EngineHarness &harness)
{
	for (const bool q : {false, true})
		for (unsigned size = 0; size < 3U; ++size) {
			auto initial = test::state(size * 31U + q * 17U);
			initial.vectors[registers.source].bytes.fill(std::byte{});
			const std::size_t lane_size = std::size_t{1} << size;
			const std::size_t base = q ? 8U : 0U;
			for (std::size_t offset = base, lane = 0;
			     offset < base + 8U;
			     offset += lane_size, ++lane)
				test::write_lane(
					initial.vectors[registers.source].bytes,
					offset, lane_size, lane + 1U);
			check_custom(harness,
				q ? "simd-widening-shift-upper-half" :
					"simd-widening-shift-lower-half",
				initial, false, q, true, size, 19U);
		}
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	exhaustive_register_fields(harness);
	narrow_boundaries(harness);
	upper_merge_destructive(harness);
	widening_shift_halves(harness);
	harness.print_summary("arm64-simd-two-register-narrow");
	return 0;
}
