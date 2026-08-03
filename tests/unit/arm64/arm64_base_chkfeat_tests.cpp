// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_base_hint_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace engine = uml::emulator::tests;
namespace hint = arm64_hint_test;

namespace {

constexpr auto instruction = hint::primary_instruction(40);
inline const auto native_chk =
	engine::NativeRequirements::features(::uml::cpu::ArmFeature::chk);

void encoding()
{
	hint::require(instruction == 0xd503'251fU,
		"CHKFEAT encoding differs from the specification");
}

void check_value(engine::EngineHarness &harness, std::uint64_t value,
	unsigned pattern)
{
	auto initial = hint::seeded_state(
		0x7600'0000'0000'0000ULL |
		(static_cast<std::uint64_t>(pattern) << 12));
	initial.registers[16] = value;
	char name[128];
	std::snprintf(name, sizeof(name),
		"chkfeat-feature-absent-pattern%u-value=%#llx",
		pattern, static_cast<unsigned long long>(value));
	harness.check_one(name, instruction, initial, initial, native_chk);
}

void every_selector_bit_and_boundary_pattern(engine::EngineHarness &harness)
{
	unsigned pattern = 0;
	check_value(harness, 0, pattern++);
	check_value(harness, ~std::uint64_t{0}, pattern++);
	check_value(harness, 0xaaaa'aaaa'aaaa'aaaaULL, pattern++);
	check_value(harness, 0x5555'5555'5555'5555ULL, pattern++);
	for (unsigned bit = 0; bit < 64; ++bit)
		check_value(harness, std::uint64_t{1} << bit, pattern++);
	hint::require(pattern == 68,
		"CHKFEAT selector-pattern count differs from the oracle");
}

} // namespace

int main()
{
	encoding();
	engine::EngineHarness harness;
	every_selector_bit_and_boundary_pattern(harness);
	const auto &counts = harness.counts();
	hint::require(counts.interpreter == 68,
		"CHKFEAT interpreter case count");
	hint::require(counts.gadgets == 68,
		"CHKFEAT gadget case count");
	hint::require(counts.jit + counts.jit_skipped == 68,
		"CHKFEAT JIT case count");
	hint::require(counts.jit_unavailable == 0,
		"CHKFEAT JIT unexpectedly unavailable");
	harness.print_summary("arm64-base-chkfeat");
}
