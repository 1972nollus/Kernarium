// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_ordered_test_support.hpp"

#include <array>
#include <cstdio>

namespace ordered = arm64_simd_ordered_test;
namespace engine = uml::emulator::tests;

namespace {

void check_allocated(engine::EngineHarness &harness,
	const ordered::SimdVariant &variant, int offset, unsigned rn,
	unsigned rt, std::uint64_t seed)
{
	auto initial = ordered::seeded_state(seed);
	ordered::set_base(initial, rn,
		ordered::unmapped_address -
			(variant.kind == ordered::SimdKind::unscaled ?
				static_cast<std::uint64_t>(
					static_cast<std::int64_t>(offset)) : 0U));
	char name[128];
	std::snprintf(name, sizeof(name),
		"%.*s allocated offset=%d rn=%u rt=%u",
		static_cast<int>(variant.name.size()), variant.name.data(),
		offset, rn, rt);
	harness.check({
		.name = name,
		.instruction =
			ordered::simd_instruction(variant, offset, rn, rt),
		.initial = initial,
		.expected = initial,
		.event = engine::ExpectedEvent::memory_fault,
		.native = ordered::native(variant),
		.fault = {
			.address = ordered::unmapped_address,
			.access = variant.load ?
				ordered::arm64::MemoryAccess::read :
				ordered::arm64::MemoryAccess::write,
		},
		.compare_fault = true,
	});
}

void every_immediate_and_register_field(engine::EngineHarness &harness)
{
	std::uint64_t seed = 0xa100'0000'0000'0000ULL;
	for (const auto &variant : ordered::simd_unscaled_variants) {
		for (unsigned imm9 = 0; imm9 < 512U; ++imm9) {
			const auto offset = static_cast<int>(
				static_cast<std::int16_t>(imm9 << 7) >> 7);
			check_allocated(harness, variant, offset, 2, 3, seed++);
		}
		for (unsigned rn = 0; rn < 32U; ++rn)
			check_allocated(harness, variant, 0, rn, 3, seed++);
		for (unsigned rt = 0; rt < 32U; ++rt)
			check_allocated(harness, variant, 0, 2, rt, seed++);
	}
	for (const auto &variant : ordered::simd_lane_variants) {
		for (unsigned rn = 0; rn < 32U; ++rn)
			check_allocated(harness, variant, 0, rn, 3, seed++);
		for (unsigned rt = 0; rt < 32U; ++rt)
			check_allocated(harness, variant, 0, 2, rt, seed++);
	}
}

void reserved_vector_size_partitions(engine::EngineHarness &harness)
{
	const auto initial =
		ordered::seeded_state(0xa200'0000'0000'0000ULL);
	for (unsigned size = 1; size < 4U; ++size)
		for (const unsigned operation : {2U, 3U}) {
			for (unsigned imm9 = 0; imm9 < 512U; ++imm9) {
				const auto instruction = 0x1d00'0800U |
					(size << 30) | (operation << 22) |
					(imm9 << 12) | (2U << 5) | 3U;
				char name[112];
				std::snprintf(name, sizeof(name),
					"reserved SIMD ordered size=%u opc=%u imm9=%u",
					size, operation, imm9);
				harness.check_illegal(name, instruction, initial,
					engine::NativeRequirements::features({
						::uml::cpu::ArmFeature::floating_point,
						::uml::cpu::ArmFeature::lrcpc3,
					}));
			}
			for (unsigned field = 0; field < 32U; ++field) {
				const auto fixed = 0x1d00'0800U |
					(size << 30) | (operation << 22);
				char rn_name[104];
				std::snprintf(rn_name, sizeof(rn_name),
					"reserved SIMD ordered size=%u opc=%u rn=%u",
					size, operation, field);
				harness.check_illegal(rn_name,
					fixed | (field << 5) | 3U, initial);
				char rt_name[104];
				std::snprintf(rt_name, sizeof(rt_name),
					"reserved SIMD ordered size=%u opc=%u rt=%u",
					size, operation, field);
				harness.check_illegal(rt_name,
					fixed | (2U << 5) | field, initial);
			}
		}
}

} // namespace

int main()
{
	engine::EngineHarness harness;
	every_immediate_and_register_field(harness);
	reserved_vector_size_partitions(harness);
	harness.print_summary("arm64-simd-ordered-allocation");
	return 0;
}
