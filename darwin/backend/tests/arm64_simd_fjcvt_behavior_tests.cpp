// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fjcvt_test_support.hpp"

namespace test = arm64_simd_fjcvt_test;
namespace base = arm64_simd_test;

namespace {

struct Case final {
	std::uint64_t input;
	std::uint32_t output;
	std::uint32_t nzcv;
	std::uint32_t exceptions;
};

void truncation_modulo_and_zero_flag()
{
	constexpr std::array cases{
		Case{0x0000'0000'0000'0000ULL, 0U, 0x4000'0000U, 0U},
		Case{0x8000'0000'0000'0000ULL, 0U, 0U, 0U},
		Case{0x3ff8'0000'0000'0000ULL, 1U, 0U, 1U << 4},
		Case{0xbff8'0000'0000'0000ULL, 0xffff'ffffU, 0U, 1U << 4},
		Case{0x41e0'0000'0000'0000ULL, 0x8000'0000U, 0U, 1U},
		Case{0x41f0'0000'0000'0000ULL, 0U, 0U, 1U},
		Case{0x41f0'0000'0010'0000ULL, 1U, 0U, 1U},
		Case{0xc1f0'0000'0010'0000ULL, 0xffff'ffffU, 0U, 1U},
		Case{0x43f0'0000'0000'0000ULL, 0U, 0U, 1U},
	};
	base::Harness harness;
	for (const auto value : cases) {
		auto initial = base::state(static_cast<unsigned>(value.input));
		test::set_source(initial, value.input);
		test::require_result(28U, value.output, value.nzcv,
			value.exceptions, initial, harness);
	}
}

} // namespace

int main()
{
	truncation_modulo_and_zero_flag();
	return 0;
}
