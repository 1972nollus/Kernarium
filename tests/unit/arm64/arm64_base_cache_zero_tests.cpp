// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_cache_zero_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstdlib>
#include <span>

namespace test = arm64_base_cache_zero_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

void every_register_and_alignment_bit()
{
	test::engine_tests::EngineHarness harness;
	for (unsigned source = 0; source < 31; ++source)
		for (unsigned low = 0; low < test::block_size; ++low) {
			const auto initial_page = test::filled_page();
			const auto expected_page =
				test::zeroed_page(test::block_offset);
			const test::engine_tests::MemoryRegion region{
				.address = test::engine_tests::engine_test_data_base,
				.initial = initial_page,
				.expected = expected_page,
			};
			auto initial = test::state(source * test::block_size + low);
			initial.registers[source] =
				test::engine_tests::engine_test_data_base +
				test::block_offset + low;
			harness.check({
				.name = "cache-zero register and alignment",
				.instruction = test::instruction(source),
				.initial = initial,
				.expected = initial,
				.native = test::native_requirements(),
				.memory =
					std::span<const test::engine_tests::MemoryRegion>{
						&region, 1},
			});
		}

	for (const std::size_t address_offset :
	     {std::size_t{0},
	      static_cast<std::size_t>(DARWIN_UML_GUEST_PAGE_SIZE - 1)}) {
		const auto initial_page = test::filled_page();
		const auto expected_page = test::zeroed_page(
			address_offset & ~(test::block_size - 1U));
		const test::engine_tests::MemoryRegion region{
			.address = test::engine_tests::engine_test_data_base,
			.initial = initial_page,
			.expected = expected_page,
		};
		auto initial = test::state(static_cast<unsigned>(address_offset));
		initial.registers[7] =
			test::engine_tests::engine_test_data_base + address_offset;
		harness.check({
			.name = "cache-zero page boundary block",
			.instruction = test::instruction(7),
			.initial = initial,
			.expected = initial,
			.native = test::native_requirements(),
			.memory = std::span<const test::engine_tests::MemoryRegion>{
				&region, 1},
		});
	}
	harness.print_summary("arm64-base-cache-zero");
}

void production_specializes_source()
{
	constexpr auto pc = test::engine_tests::engine_test_code_base;
	const auto reg = gadgets::Builder{}.build(
		pc, std::array{test::instruction(0), 0xd400'0001U});
	const auto zero = gadgets::Builder{}.build(
		pc, std::array{test::instruction(31), 0xd400'0001U});
	if (!reg.complete || !zero.complete ||
	    reg.program.words().front() == zero.program.words().front())
		std::abort();
}

} // namespace

int main()
{
	every_register_and_alignment_bit();
	production_specializes_source();
	return 0;
}
