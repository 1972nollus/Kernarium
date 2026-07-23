// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_ordered_test_support.hpp"

namespace test = arm64_simd_ordered_test;
namespace arm64 = uml::backend::arm64;

namespace {

void every_unscaled_immediate_is_allocated()
{
	for (const auto &variant : test::variants)
		for (unsigned imm9 = 0; imm9 < 512U; ++imm9) {
			test::common::ProbeMemory memory;
			arm64::CpuState cpu{};
			test::initialize(cpu);
			const auto offset = test::common::signed_imm9(imm9);
			cpu.registers[2] = test::base -
				static_cast<std::uint64_t>(offset);
			const auto result = arm64::Dispatcher{}.execute(cpu, memory,
				test::unscaled(variant.size_code, variant.opc, imm9, 2, 3));
			test::require(result.reason == arm64::StopReason::continued);
			test::require(memory.last_address == test::base);
			test::require(memory.last_size == variant.size);
		}
}

void every_lane_register_encoding_is_allocated()
{
	for (const bool load : {false, true})
		for (const bool upper : {false, true})
			for (unsigned rn = 0; rn < 32U; ++rn)
				for (unsigned rt = 0; rt < 32U; ++rt) {
					test::common::ProbeMemory memory;
					arm64::CpuState cpu{};
					test::initialize(cpu);
					if (rn == 31U)
						cpu.sp = test::base;
					else
						cpu.registers[rn] = test::base;
					const auto result = arm64::Dispatcher{}.execute(cpu, memory,
						test::lane(load, upper, rn, rt));
					test::require(result.reason ==
						arm64::StopReason::continued);
					test::require(memory.last_address == test::base);
					test::require(memory.last_size == 8U);
				}
}

void reserved_q_combinations_are_early_illegal()
{
	for (unsigned size = 1; size < 4U; ++size)
		for (const unsigned opc : {2U, 3U})
			for (unsigned imm9 = 0; imm9 < 512U; ++imm9) {
				test::common::ProbeMemory memory;
				arm64::CpuState cpu{};
				test::initialize(cpu);
				const auto before = cpu;
				const auto instruction =
					test::unscaled(size, opc, imm9, 31, 31);
				const auto result = arm64::Dispatcher{}.execute(
					cpu, memory, instruction);
				test::require(result.reason ==
					arm64::StopReason::illegal_instruction);
				test::require(memory.reads == 0U && memory.writes == 0U);
				test::require_state_unchanged(cpu, before);

				const std::array stream{instruction, 0xd400'0001U};
				const auto built = test::gadgets::Builder{}.build(
					test::pc, stream);
				test::require(built.complete);
			}
}

} // namespace

int main()
{
	every_unscaled_immediate_is_allocated();
	every_lane_register_encoding_is_allocated();
	reserved_q_combinations_are_early_illegal();
}
