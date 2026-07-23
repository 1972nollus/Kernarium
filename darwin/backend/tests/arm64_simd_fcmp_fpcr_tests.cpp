// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_fcmp_test_support.hpp"

#include <array>

namespace test = arm64_simd_fcmp_test;
namespace base = arm64_simd_test;

namespace {

struct ControlCase final {
	std::uint32_t fpcr;
	std::uint32_t positive_nzcv;
	std::uint32_t exceptions;
};

void half_precision_fz16_and_afp_controls()
{
	base::Harness harness;
	const std::array cases{
		ControlCase{0U, 0x2000'0000U, 0U},
		ControlCase{1U, 0x2000'0000U, 0U},
		ControlCase{2U, 0x2000'0000U, 0U},
		ControlCase{3U, 0x2000'0000U, 0U},
		ControlCase{1U << 19, 0x6000'0000U, 0U},
		ControlCase{(1U << 19) | 3U, 0x6000'0000U, 0U},
	};
	for (const auto &entry : cases)
		for (const bool negative : {false, true}) {
			auto initial = base::state(entry.fpcr + negative);
			initial.fpcr = entry.fpcr;
			initial.fpsr = 0x10U;
			test::put(initial, 29U, 2U,
				test::minimum_subnormal(2U, negative));
			test::require_result(test::direct_instruction(2U, false, true,
				29U), negative && entry.positive_nzcv != 0x6000'0000U ?
					0x8000'0000U : entry.positive_nzcv,
				entry.exceptions, initial, harness);
		}
}

void single_and_double_afp_subnormal_matrix()
{
	base::Harness harness;
	const std::array cases{
		ControlCase{0U, 0x2000'0000U, 0U},
		ControlCase{1U << 24, 0x6000'0000U, 1U << 7},
		ControlCase{1U, 0x6000'0000U, 0U},
		ControlCase{(1U << 24) | 1U, 0x6000'0000U, 1U << 7},
		ControlCase{2U, 0x2000'0000U, 1U << 7},
		ControlCase{(1U << 24) | 2U, 0x2000'0000U, 1U << 7},
		ControlCase{3U, 0x6000'0000U, 0U},
		ControlCase{(1U << 24) | 3U, 0x6000'0000U, 0U},
	};
	for (const unsigned bytes : {4U, 8U})
		for (const auto &entry : cases)
			for (const bool negative : {false, true}) {
				auto initial = base::state(bytes + entry.fpcr + negative);
				initial.fpcr = entry.fpcr;
				initial.fpsr = 0x0800'0010U;
				test::put(initial, 29U, bytes,
					test::minimum_subnormal(bytes, negative));
				const auto expected = negative &&
					entry.positive_nzcv != 0x6000'0000U ?
					0x8000'0000U : entry.positive_nzcv;
				test::require_result(test::direct_instruction(bytes,
					false, true, 29U), expected, entry.exceptions,
					initial, harness);
			}
}

void nan_short_circuit_and_unpack_exceptions()
{
	base::Harness harness;
	for (const unsigned bytes : {4U, 8U}) {
		/* Ordinary FZ raises IDC while unpacking the denormal even though the
		 * comparison later discovers a quiet NaN. */
		auto fz = base::state(bytes);
		fz.fpcr = 1U << 24;
		fz.fpsr = 0x10U;
		test::put(fz, 29U, bytes, test::quiet_nan(bytes));
		test::put(fz, 30U, bytes, test::minimum_subnormal(bytes));
		test::require_result(test::direct_instruction(bytes, false, false,
			29U, 30U), 0x3000'0000U, 1U << 7, fz, harness);

		/* AH defers IDC to FPProcessDenorms, which is skipped for unordered
		 * comparisons. A signaling compare therefore adds IOC only. */
		auto ah = base::state(bytes + 1U);
		ah.fpcr = 2U;
		ah.fpsr = 0x80U;
		test::put(ah, 29U, bytes, test::quiet_nan(bytes));
		test::put(ah, 30U, bytes, test::minimum_subnormal(bytes));
		test::require_result(test::direct_instruction(bytes, true, false,
			29U, 30U), 0x3000'0000U, 1U, ah, harness);
	}
}

} // namespace

int main()
{
	half_precision_fz16_and_afp_controls();
	single_and_double_afp_subnormal_matrix();
	nan_short_circuit_and_unpack_exceptions();
	return 0;
}
