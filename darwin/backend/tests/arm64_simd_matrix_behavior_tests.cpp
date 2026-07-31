// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_matrix_test_support.hpp"

#include <array>

namespace test = arm64_simd_matrix_test;

namespace {

[[nodiscard]] test::arm64::CpuState edge_state(unsigned seed,
	test::Registers registers, unsigned pattern)
{
	auto initial = test::state(seed);
	auto &destination = initial.vectors[registers.destination].bytes;
	auto &left = initial.vectors[registers.left].bytes;
	auto &right = initial.vectors[registers.right].bytes;
	switch (pattern) {
	case 0:
		break;
	case 1:
		left.fill(std::byte{0x80});
		right.fill(std::byte{0xff});
		destination.fill(std::byte{0});
		break;
	case 2:
		left.fill(std::byte{0xff});
		right.fill(std::byte{0x7f});
		destination.fill(std::byte{0xff});
		break;
	case 3:
		left.fill(std::byte{0x7f});
		right.fill(std::byte{0x80});
		for (unsigned lane = 0; lane < 4U; ++lane)
			test::dot::write_word(destination, lane * 4U,
				0x7fff'fff0U + lane * 0x1000'0000U);
		break;
	case 4:
		for (unsigned byte = 0; byte < 16U; ++byte) {
			left[byte] = static_cast<std::byte>(byte + 1U);
			right[byte] = static_cast<std::byte>(
				byte < 8U ? 1U << (byte & 7U) :
				0x80U >> (byte & 7U));
		}
		for (unsigned lane = 0; lane < 4U; ++lane)
			test::dot::write_word(destination, lane * 4U,
				0x1020'3040U * (lane + 1U));
		break;
	case 5:
		for (unsigned byte = 0; byte < 16U; ++byte) {
			left[byte] = static_cast<std::byte>(
				byte % 3U == 0U ? 0x80U :
				byte % 3U == 1U ? 0xffU : 0x7fU);
			right[byte] = static_cast<std::byte>(
				byte % 4U == 0U ? 0xffU :
				byte % 4U == 1U ? 0x80U : byte * 19U + 7U);
		}
		break;
	default:
		test::fail("unknown matrix edge pattern");
	}
	return initial;
}

void every_signedness_layout_boundary_and_alias()
{
	test::engine_tests::EngineHarness harness;
	constexpr std::array register_sets{
		test::Registers{28U, 29U, 30U},
		test::Registers{1U, 1U, 2U},
		test::Registers{2U, 1U, 2U},
		test::Registers{1U, 2U, 2U},
		test::Registers{3U, 3U, 3U},
	};
	for (const auto mode : test::modes)
		for (const auto registers : register_sets)
			for (unsigned pattern = 0; pattern < 6U; ++pattern)
				test::check(harness, "matrix behavior", mode, registers,
					edge_state(pattern * 31U +
						static_cast<unsigned>(mode) * 7U +
						registers.destination,
						registers, pattern));
	harness.print_summary("simd-matrix-behavior");
}

} // namespace

int main()
{
	every_signedness_layout_boundary_and_alias();
	return 0;
}
