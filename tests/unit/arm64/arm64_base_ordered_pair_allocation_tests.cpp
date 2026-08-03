// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_ordered_memory_test_support.hpp"

#include <cstdio>

namespace ordered = arm64_ordered_memory_test;
namespace engine = uml::emulator::tests;

namespace {

[[nodiscard]] constexpr bool duplicate_load(
	const ordered::PairVariant &variant, unsigned rt, unsigned rt2)
{
	return variant.load && rt == rt2 && rt != 31U;
}

void check_allocated_fields(engine::EngineHarness &harness,
	const ordered::PairVariant &variant, unsigned rn, unsigned rt,
	unsigned rt2, std::uint64_t seed)
{
	auto initial = ordered::seeded_state(seed);
	ordered::set_base(initial, rn, ordered::unmapped_address);
	char name[128];
	std::snprintf(name, sizeof(name),
		"%.*s allocated rn=%u rt=%u rt2=%u",
		static_cast<int>(variant.name.size()), variant.name.data(),
		rn, rt, rt2);
	if (duplicate_load(variant, rt, rt2)) {
		harness.check({
			.name = name,
			.instruction =
				ordered::pair_instruction(variant, rn, rt, rt2),
			.initial = initial,
			.expected = initial,
			.event = engine::ExpectedEvent::illegal_instruction,
			.native = ordered::native(variant),
			.invalid_encoding = true,
		});
		return;
	}
	const auto total = 2U * (std::size_t{1} << variant.size_code);
	const auto target =
		variant.writeback && !variant.load ?
			ordered::unmapped_address - total :
			ordered::unmapped_address;
	harness.check({
		.name = name,
		.instruction =
			ordered::pair_instruction(variant, rn, rt, rt2),
		.initial = initial,
		.expected = initial,
		.event = engine::ExpectedEvent::memory_fault,
		.native = ordered::native(variant),
		.fault = {
			.address = target,
			.access = variant.load ?
				ordered::arm64::MemoryAccess::read :
				ordered::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
	});
}

void every_register_field(engine::EngineHarness &harness)
{
	std::uint64_t seed = 0x7100'0000'0000'0000ULL;
	for (const auto &variant : ordered::pair_variants) {
		for (unsigned rn = 0; rn < 32U; ++rn)
			check_allocated_fields(harness, variant, rn, 3, 4, seed++);
		for (unsigned rt = 0; rt < 32U; ++rt)
			check_allocated_fields(harness, variant, 2, rt, 4, seed++);
		for (unsigned rt2 = 0; rt2 < 32U; ++rt2)
			check_allocated_fields(harness, variant, 2, 3, rt2, seed++);
		check_allocated_fields(harness, variant, 31, 31, 31, seed++);
	}
}

[[nodiscard]] constexpr bool legal_allocation(std::uint32_t instruction)
{
	switch (instruction & 0xffe0'fc00U) {
	case 0x9900'0800U:
	case 0xd900'0800U:
	case 0x9900'1800U:
	case 0xd900'1800U:
	case 0x9940'0800U:
	case 0xd940'0800U:
	case 0x9940'1800U:
	case 0xd940'1800U:
	case 0xd900'5800U:
	case 0xd940'5800U:
	case 0xd940'7800U:
		return true;
	default:
		return false;
	}
}

void every_reserved_opcode_partition(engine::EngineHarness &harness)
{
	const auto initial =
		ordered::seeded_state(0x7200'0000'0000'0000ULL);
	for (unsigned size = 0; size < 4U; ++size)
		for (const bool load : {false, true})
			for (unsigned opc2 = 0; opc2 < 16U; ++opc2) {
				const auto instruction = 0x1900'0800U |
					(size << 30) |
					(static_cast<unsigned>(load) << 22) |
					(4U << 16) | (opc2 << 12) |
					(2U << 5) | 3U;
				if (legal_allocation(instruction))
					continue;
				char name[112];
				std::snprintf(name, sizeof(name),
					"reserved ordered pair size=%u load=%u opc2=%u",
					size, static_cast<unsigned>(load), opc2);
				harness.check_illegal(name, instruction, initial);
			}
}

void reserved_fields_do_not_observe_state(engine::EngineHarness &harness)
{
	const auto initial =
		ordered::seeded_state(0x7300'0000'0000'0000ULL);
	/* size=00, load=0, opc2=0000 is permanently unallocated. */
	for (unsigned rn = 0; rn < 32U; ++rn)
		for (const unsigned rt : {0U, 15U, 31U})
			for (const unsigned rt2 : {0U, 15U, 31U}) {
				const auto instruction = 0x1900'0800U |
					(rt2 << 16) | (rn << 5) | rt;
				char name[112];
				std::snprintf(name, sizeof(name),
					"reserved ordered pair fields rn=%u rt=%u rt2=%u",
					rn, rt, rt2);
				harness.check_illegal(name, instruction, initial);
			}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	every_register_field(harness);
	every_reserved_opcode_partition(harness);
	reserved_fields_do_not_observe_state(harness);
	harness.print_summary("arm64-base-ordered-pair-allocation");
	return 0;
}
