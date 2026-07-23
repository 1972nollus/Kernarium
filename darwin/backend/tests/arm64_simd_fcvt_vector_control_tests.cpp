// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcvt_vector_test_support.hpp"

namespace test = arm64_simd_fcvt_vector_test;
namespace base = arm64_simd_test;

namespace {

void nan_payload_default_nan_and_signaling()
{
	base::Harness harness;
	const auto long_shape = test::shapes[0];
	for (const bool signaling : {false, true}) {
		auto initial = base::state(signaling);
		initial.fpsr = 0x0800'0090U;
		std::array<std::uint64_t, 4> outputs{};
		for (unsigned lane = 0; lane < 4U; ++lane) {
			test::set_source_lane(initial, 29U, long_shape, false, lane,
				signaling ? 0x7d51U : 0x7e55U);
			outputs[lane] = signaling ? 0x7fea'2000U : 0x7fca'a000U;
		}
		test::require_result(test::instruction(long_shape, false, 28U, 29U),
			long_shape, false, 28U, outputs, signaling ? 1U : 0U,
			initial, harness);
	}

	const auto narrow_shape = test::shapes[2];
	for (const bool ah : {false, true}) {
		auto initial = base::state(10U + ah);
		initial.fpcr = (1U << 25) | (ah ? 2U : 0U);
		std::array<std::uint64_t, 4> outputs{};
		for (unsigned lane = 0; lane < 4U; ++lane) {
			test::set_source_lane(initial, 29U, narrow_shape, false, lane,
				0xffc1'2345U);
			outputs[lane] = ah ? 0xfe00U : 0x7e00U;
		}
		test::require_result(test::instruction(narrow_shape, false, 28U, 29U),
			narrow_shape, false, 28U, outputs, 0U, initial, harness);
	}
}

void afp_source_denormal_matrix()
{
	base::Harness harness;
	struct Entry final {
		std::uint32_t fpcr;
		std::uint64_t output;
		std::uint32_t exceptions;
	};
	const Entry entries[]{
		{0U, 0x36a0'0000'0000'0000ULL, 0U},
		{1U << 24, 0U, 1U << 7},
		{1U, 0U, 0U},
		{2U, 0x36a0'0000'0000'0000ULL, 1U << 7},
		{3U, 0U, 0U},
	};
	const auto shape = test::shapes[1];
	for (const auto entry : entries) {
		auto initial = base::state(entry.fpcr);
		initial.fpcr = entry.fpcr;
		initial.fpsr = 0x10U;
		std::array<std::uint64_t, 4> outputs{};
		for (unsigned lane = 0; lane < 2U; ++lane) {
			test::set_source_lane(initial, 29U, shape, false, lane, 1U);
			outputs[lane] = entry.output;
		}
		test::require_result(test::instruction(shape, false, 28U, 29U),
			shape, false, 28U, std::span{outputs}.first(2U),
			entry.exceptions, initial, harness);
	}
}

void alternative_half_source_and_destination()
{
	base::Harness harness;
	for (const bool source_half : {false, true}) {
		const auto shape = source_half ? test::shapes[0] : test::shapes[2];
		auto initial = base::state(100U + source_half);
		initial.fpcr = 1U << 26;
		const unsigned count = test::lanes(shape);
		std::array<std::uint64_t, 4> outputs{};
		for (unsigned lane = 0; lane < count; ++lane) {
			test::set_source_lane(initial, 29U, shape, false, lane,
				source_half ? 0x7c00U : 0x4780'0000U);
			outputs[lane] = source_half ? 0x4780'0000U : 0x7c00U;
		}
		test::require_result(test::instruction(shape, false, 28U, 29U),
			shape, false, 28U, std::span{outputs}.first(count), 0U,
			initial, harness);
	}
}

} // namespace

int main()
{
	nan_payload_default_nan_and_signaling();
	afp_source_denormal_matrix();
	alternative_half_source_and_destination();
	return 0;
}
