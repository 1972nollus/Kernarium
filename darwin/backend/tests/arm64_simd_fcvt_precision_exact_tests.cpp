// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvt_precision_test_support.hpp"

#include <array>

namespace test = arm64_simd_fcvt_precision_test;
namespace base = arm64_simd_test;

namespace {

struct ExactCase final {
	unsigned source_width;
	unsigned destination_width;
	std::uint64_t input;
	std::uint64_t output;
};

void exact_finite_zero_and_infinity_values()
{
	base::Harness harness;
	const std::array cases{
		ExactCase{16U, 32U, 0x0000U, 0x0000'0000U},
		ExactCase{16U, 32U, 0x8000U, 0x8000'0000U},
		ExactCase{16U, 32U, 0x3c00U, 0x3f80'0000U},
		ExactCase{16U, 32U, 0x0001U, 0x3380'0000U},
		ExactCase{16U, 64U, 0x3c00U, 0x3ff0'0000'0000'0000ULL},
		ExactCase{16U, 64U, 0x0001U, 0x3e70'0000'0000'0000ULL},
		ExactCase{32U, 64U, 0x3f80'0000U, 0x3ff0'0000'0000'0000ULL},
		ExactCase{32U, 64U, 0x0000'0001U, 0x36a0'0000'0000'0000ULL},
		ExactCase{32U, 16U, 0x3f80'0000U, 0x3c00U},
		ExactCase{64U, 32U, 0x3ff0'0000'0000'0000ULL, 0x3f80'0000U},
		ExactCase{64U, 16U, 0x3ff0'0000'0000'0000ULL, 0x3c00U},
	};
	for (unsigned index = 0; index < cases.size(); ++index) {
		const auto entry = cases[index];
		auto initial = base::state(index);
		initial.fpsr = 0x0800'0090U;
		test::set_source(initial, 29U, entry.source_width, entry.input);
		test::require_result(test::instruction(entry.source_width,
			entry.destination_width, 28U, 29U), entry.destination_width,
			28U, entry.output, 0U, initial, harness);
	}
	for (const auto pair : std::array{
			std::pair{16U, 32U}, std::pair{16U, 64U},
			std::pair{32U, 16U}, std::pair{32U, 64U},
			std::pair{64U, 16U}, std::pair{64U, 32U}})
		for (const bool negative : {false, true}) {
			auto initial = base::state(pair.first + pair.second + negative);
			initial.fpsr = 0x10U;
			test::set_source(initial, 29U, pair.first,
				test::infinity(pair.first, negative));
			test::require_result(test::instruction(pair.first, pair.second,
				28U, 29U), pair.second, 28U,
				test::infinity(pair.second, negative), 0U, initial, harness);
		}
}

void representative_default_path_differential_samples()
{
	base::Harness harness;
	std::uint64_t random = 0x243f'6a88'85a3'08d3ULL;
	for (const auto pair : std::array{
			std::pair{16U, 32U}, std::pair{16U, 64U},
			std::pair{32U, 16U}, std::pair{32U, 64U},
			std::pair{64U, 16U}, std::pair{64U, 32U}})
		for (unsigned mode = 0; mode < 4U; ++mode)
			for (unsigned sample = 0; sample < 256U; ++sample) {
				random ^= random << 13;
				random ^= random >> 7;
				random ^= random << 17;
				auto initial = base::state(sample + mode);
				initial.fpcr = mode << 22;
				initial.fpsr = sample & 1U ? 0x80U : 0U;
				test::set_source(initial, 29U, pair.first, random);
				harness.require_differential(test::instruction(pair.first,
					pair.second, 28U, 29U), initial);
			}
}

} // namespace

int main()
{
	exact_finite_zero_and_infinity_values();
	representative_default_path_differential_samples();
	return 0;
}
