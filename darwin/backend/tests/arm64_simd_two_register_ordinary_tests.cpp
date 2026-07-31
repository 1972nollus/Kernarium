// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_two_register_test_support.hpp"

#include <array>
#include <cstdio>

namespace test = arm64_simd_two_register_test;

namespace {

constexpr std::array ordinary_opcodes{3U, 4U, 7U, 11U};
constexpr test::Registers registers{0U, 1U};

void check_custom(test::engine_tests::EngineHarness &harness,
	const char *name, const test::arm64::CpuState &initial,
	bool scalar, bool q, bool unsigned_group, unsigned size, unsigned opcode)
{
	harness.check_one(name,
		test::instruction(scalar, q, unsigned_group, size, opcode,
			registers),
		initial,
		test::expected_state(initial, scalar, q, unsigned_group, size,
			opcode, registers),
		test::native_requirements());
}

void exhaustive_register_fields(test::engine_tests::EngineHarness &harness)
{
	test::for_each_allocated_form(ordinary_opcodes,
		[&](bool scalar, bool q, bool unsigned_group,
		    unsigned size, unsigned opcode) {
			test::check_form(harness, "simd-two-register-ordinary",
				scalar, q, unsigned_group, size, opcode);
		});
}

void unary_minimum_edges(test::engine_tests::EngineHarness &harness)
{
	constexpr std::array unary_opcodes{7U, 11U};
	test::for_each_allocated_form(unary_opcodes,
		[&](bool scalar, bool q, bool unsigned_group,
		    unsigned size, unsigned opcode) {
			auto initial = test::state(
				opcode * 41U + size * 13U +
				unsigned_group * 7U + scalar * 3U + q);
			initial.vectors[registers.source].bytes.fill(std::byte{});
			const std::size_t lane_size = std::size_t{1} << size;
			const unsigned lane_bits =
				static_cast<unsigned>(lane_size * 8U);
			const std::size_t active =
				scalar ? lane_size : q ? 16U : 8U;
			for (std::size_t offset = 0; offset < active;
			     offset += lane_size)
				test::write_lane(initial.vectors[registers.source].bytes,
					offset, lane_size,
					std::uint64_t{1} << (lane_bits - 1U));
			check_custom(harness,
				opcode == 7U ?
					"simd-saturating-unary-minimum" :
					"simd-wrapping-unary-minimum",
				initial, scalar, q, unsigned_group, size, opcode);

			initial.vectors[registers.source].bytes.fill(std::byte{});
			initial.fpsr |= 1U << 27;
			check_custom(harness,
				opcode == 7U ?
					"simd-saturating-unary-qc-sticky" :
					"simd-wrapping-unary-preserves-fpsr",
				initial, scalar, q, unsigned_group, size, opcode);
		});
}

void mixed_accumulate_edges(test::engine_tests::EngineHarness &harness)
{
	constexpr std::array mixed_opcode{3U};
	test::for_each_allocated_form(mixed_opcode,
		[&](bool scalar, bool q, bool unsigned_group,
		    unsigned size, unsigned opcode) {
			auto initial = test::state(
				size * 29U + unsigned_group * 17U +
				scalar * 11U + q * 5U);
			initial.vectors[registers.destination].bytes.fill(std::byte{});
			initial.vectors[registers.source].bytes.fill(std::byte{});
			const std::size_t lane_size = std::size_t{1} << size;
			const unsigned lane_bits =
				static_cast<unsigned>(lane_size * 8U);
			const std::size_t active =
				scalar ? lane_size : q ? 16U : 8U;
			for (std::size_t offset = 0; offset < active;
			     offset += lane_size) {
				if (unsigned_group) {
					test::write_lane(
						initial.vectors[registers.destination].bytes,
						offset, lane_size, 0);
					test::write_lane(
						initial.vectors[registers.source].bytes,
						offset, lane_size,
						std::uint64_t{1} << (lane_bits - 1U));
				} else {
					test::write_lane(
						initial.vectors[registers.destination].bytes,
						offset, lane_size,
						(std::uint64_t{1} << (lane_bits - 1U)) - 1U);
					test::write_lane(
						initial.vectors[registers.source].bytes,
						offset, lane_size, test::mask(lane_bits));
				}
			}
			check_custom(harness,
				unsigned_group ?
					"simd-unsigned-destination-lower-saturation" :
					"simd-signed-destination-upper-saturation",
				initial, scalar, q, unsigned_group, size, opcode);

			initial.vectors[registers.destination].bytes.fill(std::byte{});
			initial.vectors[registers.source].bytes.fill(std::byte{});
			initial.fpsr |= 1U << 27;
			check_custom(harness,
				"simd-mixed-accumulate-qc-sticky",
				initial, scalar, q, unsigned_group, size, opcode);
		});
}

void count_edges(test::engine_tests::EngineHarness &harness)
{
	constexpr std::array count_opcode{4U};
	test::for_each_allocated_form(count_opcode,
		[&](bool scalar, bool q, bool unsigned_group,
		    unsigned size, unsigned opcode) {
			auto initial = test::state(
				size * 23U + unsigned_group * 31U + q * 47U);
			initial.vectors[registers.source].bytes.fill(
				unsigned_group ? std::byte{} : std::byte{0xff});
			check_custom(harness,
				unsigned_group ?
					"simd-leading-zero-full-width" :
					"simd-leading-sign-full-width-minus-one",
				initial, scalar, q, unsigned_group, size, opcode);
		});
}

} // namespace

int main()
{
	test::engine_tests::EngineHarness harness;
	exhaustive_register_fields(harness);
	unary_minimum_edges(harness);
	mixed_accumulate_edges(harness);
	count_edges(harness);
	harness.print_summary("arm64-simd-two-register-ordinary");
	return 0;
}
