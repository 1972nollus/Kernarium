// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_bit_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace test = base_bit_test;
namespace engine_tests = uml::emulator::tests;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

[[noreturn]] void fail(const char *message)
{
	std::fprintf(stderr, "%s\n", message);
	std::abort();
}

void require(bool condition, const char *message)
{
	if (!condition)
		fail(message);
}

void every_allocated_register_field(engine_tests::EngineHarness &harness)
{
	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation < 32U; ++operation) {
			if (!test::allocated(wide, operation))
				continue;
			for (unsigned source = 0; source < 32U; ++source)
				for (unsigned destination = 0; destination < 32U;
				     ++destination) {
					auto initial = test::state(
						test::seed_constant ^
						(static_cast<std::uint64_t>(
							 operation)
						 << 40U) ^
						(static_cast<std::uint64_t>(
							 source)
						 << 24U) ^
						destination);
					char name[112];
					std::snprintf(name, sizeof(name),
						"base-bit-allocation width=%u op=%u rn=%u rd=%u",
						wide ? 64U : 32U, operation,
						source, destination);
					harness.check_one(name,
						test::instruction(wide,
							operation, source,
							destination),
						initial,
						test::expected_state(initial,
							wide, operation,
							source, destination),
						test::native_requirement(
							operation));
				}
		}
}

void encoding_examples()
{
	require(test::instruction(false, 0, 5, 13) == 0x5ac0'00adU,
		"RBIT W encoding mismatch");
	require(test::instruction(true, 2, 3, 9) == 0xdac0'0869U,
		"REV32 X encoding mismatch");
	require(test::instruction(false, 2, 5, 15) == 0x5ac0'08afU,
		"REV W encoding mismatch");
	require(test::instruction(true, 3, 4, 10) == 0xdac0'0c8aU,
		"REV X encoding mismatch");
	require(test::instruction(true, 8, 31, 31) == 0xdac0'23ffU,
		"optional zero/discard encoding mismatch");
}

void production_specialization()
{
	constexpr auto base = engine_tests::engine_test_code_base;
	constexpr std::uint32_t nop = 0xd503'201fU;
	const auto folded_nop = gadgets::Builder{}.build(
		base, std::array{nop});
	require(folded_nop.complete, "NOP program did not build");

	std::set<std::uintptr_t> entries;
	for (const bool wide : {false, true})
		for (unsigned operation = 0; operation < 9U; ++operation) {
			if (!test::allocated(wide, operation))
				continue;
			const auto regular = gadgets::Builder{}.build(
				base, std::array{test::instruction(
					wide, operation, 3, 7)});
			if (!regular.complete ||
			    regular.program.words().size() != 5U)
				std::fprintf(stderr,
					"unexpected ordinary stream width=%u op=%u complete=%u words=%zu\n",
					wide ? 64U : 32U, operation,
					regular.complete ? 1U : 0U,
					regular.program.words().size());
			require(regular.complete &&
					regular.program.words().size() == 5U,
				"ordinary bit operation was not lowered to one gadget and two offsets");
			require(regular.program.words()[1] ==
					offsetof(uml::cpu::arm64::CpuState,
						 registers) +
						3U * sizeof(std::uint64_t),
				"source register was not decoded at translation time");
			require(regular.program.words()[2] ==
					offsetof(uml::cpu::arm64::CpuState,
						 registers) +
						7U * sizeof(std::uint64_t),
				"destination register was not decoded at translation time");
			entries.insert(regular.program.words().front());

			const auto zero_source = gadgets::Builder{}.build(
				base, std::array{test::instruction(
					wide, operation, 31, 7)});
			require(zero_source.complete &&
					zero_source.program.words().size() == 5U,
				"zero-source bit operation has unexpected stream shape");
			require(zero_source.program.words().front() ==
					regular.program.words().front(),
				"zero source selected a redundant runtime gadget");
			require(zero_source.program.words()[1] ==
					offsetof(uml::cpu::arm64::CpuState,
						 gadget_zero),
				"zero source was not resolved at translation time");

			const auto discarded = gadgets::Builder{}.build(
				base, std::array{test::instruction(
					wide, operation, 3, 31)});
			require(discarded.complete &&
					discarded.program.words().size() == 3U,
				"discarded destination retained runtime operands");
			require(discarded.program.words().front() ==
					folded_nop.program.words().front(),
				"discarded destination was not folded to NOP");
		}
	require(entries.size() == 17U,
		"allocated width/operation forms did not select distinct gadgets");
}

} // namespace

int main()
{
	encoding_examples();
	engine_tests::EngineHarness harness;
	every_allocated_register_field(harness);
	production_specialization();
	harness.print_summary("arm64-base-bit-allocation");
	return 0;
}
