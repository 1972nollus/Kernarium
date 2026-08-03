// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_floating_test_support.hpp"

#include <array>
#include <cstdio>

namespace fp_atomic = arm64_atomic_floating_test;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::emulator::tests;

namespace {

struct Precision final {
	unsigned size_code;
	std::uint64_t sign;
	std::uint64_t one;
	std::uint64_t two;
	std::uint64_t three;
	std::uint64_t half_ulp;
	std::uint64_t next;
	std::uint64_t quiet_nan;
	std::uint64_t signaling_nan;
	std::uint64_t default_nan;
	std::uint64_t positive_infinity;
};

constexpr std::array widths{
	Precision{1, 0x8000ULL, 0x3c00, 0x4000, 0x4200, 0x1000, 0x3c01,
		0x7e15, 0x7d15, 0x7e00, 0x7c00},
	Precision{2, 0x8000'0000ULL, 0x3f80'0000, 0x4000'0000,
		0x4040'0000, 0x3380'0000, 0x3f80'0001, 0x7fc1'2345,
		0x7f81'2345, 0x7fc0'0000, 0x7f80'0000},
	Precision{3, 0x8000'0000'0000'0000ULL,
		0x3ff0'0000'0000'0000, 0x4000'0000'0000'0000,
		0x4008'0000'0000'0000, 0x3ca0'0000'0000'0000,
		0x3ff0'0000'0000'0001, 0x7ff8'1234'5678'9abc,
		0x7ff0'1234'5678'9abc, 0x7ff8'0000'0000'0000,
		0x7ff0'0000'0000'0000},
};

void exact_case(engine::EngineHarness &harness,
	fp_atomic::ExpectedCounts &counts, const Precision &shape,
	unsigned case_index, fp_atomic::Operation operation,
	std::uint64_t observed, std::uint64_t operand, std::uint32_t fpcr,
	std::uint64_t expected)
{
	for (unsigned ordering = 0; ordering < 4U; ++ordering) {
		const bool acquire = (ordering & 2U) != 0U;
		const bool release = (ordering & 1U) != 0U;
		char name[160];
		std::snprintf(name, sizeof(name),
			"precision=%u exact case=%u load A=%u R=%u",
			shape.size_code, case_index, static_cast<unsigned>(acquire),
			static_cast<unsigned>(release));
		fp_atomic::check(harness, counts, name, shape.size_code, operation,
			acquire, release, false, 2, 3, 4, atomic::data_address,
			observed, operand, expected, {}, false, false, fpcr);
	}
	for (const bool release : {false, true}) {
		char name[160];
		std::snprintf(name, sizeof(name),
			"precision=%u exact case=%u store R=%u", shape.size_code,
			case_index, static_cast<unsigned>(release));
		fp_atomic::check(harness, counts, name, shape.size_code, operation,
			false, release, true, 2, 3, 31, atomic::data_address,
			observed, operand, expected, {}, false, false, fpcr);
	}
}

void exact_precision_cases(
	engine::EngineHarness &harness, fp_atomic::ExpectedCounts &counts)
{
	for (const auto &shape : widths) {
		unsigned index = 0;
		const auto run = [&](fp_atomic::Operation operation,
			std::uint64_t observed, std::uint64_t operand,
			std::uint32_t fpcr, std::uint64_t expected) {
			exact_case(harness, counts, shape, index++, operation,
				observed, operand, fpcr, expected);
		};
		run(fp_atomic::Operation::add, shape.one, shape.two, 0, shape.three);
		run(fp_atomic::Operation::add, shape.one, shape.half_ulp, 0,
			shape.one);
		run(fp_atomic::Operation::add, shape.one, shape.half_ulp, 1U << 22,
			shape.next);
		run(fp_atomic::Operation::add, shape.positive_infinity,
			shape.positive_infinity | shape.sign, 0, shape.default_nan);
		run(fp_atomic::Operation::maximum, shape.sign, 0, 0, 0);
		run(fp_atomic::Operation::minimum, shape.sign, 0, 0, shape.sign);
		run(fp_atomic::Operation::maximum, shape.quiet_nan, shape.one,
			2U | 0x0000'9f00U, shape.default_nan);
		run(fp_atomic::Operation::minimum, shape.one, shape.quiet_nan, 0,
			shape.default_nan);
		run(fp_atomic::Operation::maximum_number, shape.quiet_nan,
			shape.one, 0, shape.one);
		run(fp_atomic::Operation::minimum_number, shape.one,
			shape.quiet_nan, 0, shape.one);
		run(fp_atomic::Operation::maximum_number, shape.signaling_nan,
			shape.one, 0, shape.default_nan);
		run(fp_atomic::Operation::minimum_number, shape.quiet_nan,
			shape.quiet_nan | shape.sign, 0, shape.default_nan);

		const std::uint32_t flush =
			shape.size_code == 1U ? 1U << 19 : 1U;
		run(fp_atomic::Operation::add, 1, 0, flush, 0);
		if (shape.size_code != 1U)
			run(fp_atomic::Operation::add, 1, 0, 1U << 24, 0);
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	fp_atomic::ExpectedCounts counts;
	exact_precision_cases(harness, counts);
	fp_atomic::require(counts.cases == 246U,
		"precision suite case-count drift");
	fp_atomic::require_counts(harness, counts);
	harness.print_summary("arm64-atomic-floating-precision");
}
