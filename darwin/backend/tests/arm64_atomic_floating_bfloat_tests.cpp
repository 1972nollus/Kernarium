// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_floating_test_support.hpp"

#include <array>
#include <cstdio>

namespace fp_atomic = arm64_atomic_floating_test;
namespace atomic = arm64_atomic_engine_test;
namespace engine = uml::backend::tests;

namespace {

struct Case final {
	fp_atomic::Operation operation;
	std::uint16_t memory;
	std::uint16_t operand;
	std::uint32_t fpcr;
	std::uint16_t expected;
};

constexpr std::array cases{
	Case{fp_atomic::Operation::add, 0x3f80, 0x4000, 0, 0x4040},
	Case{fp_atomic::Operation::add, 0x3f80, 0x3b80, 0, 0x3f80},
	Case{fp_atomic::Operation::add, 0x3f80, 0x3b80, 1U << 22, 0x3f81},
	Case{fp_atomic::Operation::add, 0xbf80, 0xbb80, 2U << 22, 0xbf81},
	Case{fp_atomic::Operation::add, 0xbf80, 0xbb80, 1U << 22, 0xbf80},
	Case{fp_atomic::Operation::add, 0x0001, 0x0000, 0, 0x0001},
	Case{fp_atomic::Operation::add, 0x0001, 0x0000, 1U << 24, 0x0000},
	Case{fp_atomic::Operation::add, 0x0001, 0x0000, 1U, 0x0000},
	Case{fp_atomic::Operation::add, 0x7f80, 0xff80, 0, 0x7fc0},
	Case{fp_atomic::Operation::add, 0x7f7f, 0x7f7f, 0, 0x7f80},
	Case{fp_atomic::Operation::add, 0x7f7f, 0x7f7f, 3U << 22, 0x7f7f},
	Case{fp_atomic::Operation::add, 0x7fa1, 0x3f80, 0, 0x7fc0},
	Case{fp_atomic::Operation::maximum, 0x8000, 0x0000, 0, 0x0000},
	Case{fp_atomic::Operation::minimum, 0x8000, 0x0000, 0, 0x8000},
	Case{fp_atomic::Operation::maximum, 0x7fc5, 0x3f80, 0, 0x7fc0},
	Case{fp_atomic::Operation::maximum, 0x7fc5, 0x3f80, 2U, 0x7fc0},
	Case{fp_atomic::Operation::minimum, 0x3f80, 0xffc5, 1U << 25, 0x7fc0},
	Case{fp_atomic::Operation::maximum_number, 0x7fc5, 0x3f80, 0, 0x3f80},
	Case{fp_atomic::Operation::maximum_number, 0x3f80, 0x7fc5, 0, 0x3f80},
	Case{fp_atomic::Operation::minimum_number, 0x7fc5, 0xbf80, 0, 0xbf80},
	Case{fp_atomic::Operation::minimum_number, 0xbf80, 0x7fc5, 0, 0xbf80},
	Case{fp_atomic::Operation::maximum_number, 0x7f81, 0x3f80, 0, 0x7fc0},
	Case{fp_atomic::Operation::minimum_number, 0x7fc1, 0xffc1, 0, 0x7fc0},
	Case{fp_atomic::Operation::maximum, 0x0001, 0x8000, 1U, 0x0000},
	Case{fp_atomic::Operation::minimum, 0x8001, 0x0000, 1U << 24, 0x8000},
};

void exact_bfloat_cases(
	engine::EngineHarness &harness, fp_atomic::ExpectedCounts &counts)
{
	for (std::size_t index = 0; index < cases.size(); ++index) {
		const auto &item = cases[index];
		for (unsigned ordering = 0; ordering < 4U; ++ordering) {
			const bool acquire = (ordering & 2U) != 0U;
			const bool release = (ordering & 1U) != 0U;
			char name[160];
			std::snprintf(name, sizeof(name),
				"bfloat exact case=%zu load A=%u R=%u", index,
				static_cast<unsigned>(acquire),
				static_cast<unsigned>(release));
			auto initial = atomic::state(
				0x3000U + static_cast<unsigned>(index) * 8U + ordering);
			initial.fpcr = item.fpcr;
			initial.fpsr = 0x0800'009fU;
			initial.registers[2] = atomic::data_address;
			fp_atomic::set_vector(initial, 3, item.operand, 2);
			fp_atomic::set_vector(initial, 4, 0x55aa, 2,
				std::byte{0x5a});

			fp_atomic::Page initial_page{};
			fp_atomic::fill_page(initial_page,
				static_cast<unsigned>(index));
			atomic::put(initial_page, atomic::data_offset, item.memory, 2);
			auto expected_page = initial_page;
			atomic::put(expected_page, atomic::data_offset, item.expected, 2);
			auto expected = initial;
			fp_atomic::set_vector(expected, 4, item.memory, 2,
				std::byte{});
			const auto memory = atomic::region(initial_page, expected_page);
			harness.check({
				.name = name,
				.instruction = fp_atomic::instruction(0, item.operation,
					acquire, release, false, 2, 3, 4),
				.initial = initial,
				.expected = expected,
				.native = fp_atomic::native(),
				.memory = std::span<const engine::MemoryRegion>{&memory, 1},
			});
			counts.add();
		}
		for (const bool release : {false, true}) {
			char name[160];
			std::snprintf(name, sizeof(name),
				"bfloat exact case=%zu store R=%u", index,
				static_cast<unsigned>(release));
			fp_atomic::check(harness, counts, name, 0, item.operation,
				false, release, true, 2, 3, 31,
				atomic::data_address, item.memory, item.operand,
				item.expected, {}, false, false, item.fpcr);
		}
	}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	fp_atomic::ExpectedCounts counts;
	exact_bfloat_cases(harness, counts);
	fp_atomic::require(counts.cases == 150U,
		"bfloat suite case-count drift");
	fp_atomic::require_counts(harness, counts);
	harness.print_summary("arm64-atomic-floating-bfloat");
}
