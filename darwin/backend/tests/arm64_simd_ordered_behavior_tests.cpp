// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_ordered_test_support.hpp"

#include <algorithm>
#include <cstring>

namespace test = arm64_simd_ordered_test;
namespace arm64 = uml::backend::arm64;

namespace {

void unscaled_load_store_shapes_and_offsets()
{
	for (const auto &variant : test::variants)
		for (const unsigned imm9 : {0U, 1U, 255U, 256U, 511U}) {
			test::common::ProbeMemory memory;
			arm64::CpuState cpu{};
			test::initialize(cpu);
			const auto source = cpu.vectors[3];
			const auto offset = test::common::signed_imm9(imm9);
			cpu.registers[2] = test::base -
				static_cast<std::uint64_t>(offset);
			const auto result = arm64::Dispatcher{}.execute(cpu, memory,
				test::unscaled(variant.size_code, variant.opc, imm9, 2, 3));
			test::require(result.reason == arm64::StopReason::continued);
			test::require(cpu.pc == test::pc + 4U);
			test::require(memory.last_address == test::base);
			if (variant.load) {
				test::require(memory.reads == 1U && memory.writes == 0U);
				test::require_loaded(cpu.vectors[3], memory, variant.size);
			} else {
				test::require(memory.reads == 0U && memory.writes == 1U);
				test::require(std::memcmp(memory.written.data(),
					source.bytes.data(), variant.size) == 0);
				test::require(cpu.vectors[3].bytes == source.bytes);
			}
		}
}

void lane_loads_preserve_the_other_half_and_stores_select_the_lane()
{
	for (const bool upper : {false, true}) {
		test::common::ProbeMemory memory;
		arm64::CpuState cpu{};
		test::initialize(cpu);
		cpu.sp = test::base;
		const auto before = cpu.vectors[31];
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			test::lane(true, upper, 31, 31)).reason ==
			arm64::StopReason::continued);
		const auto offset = upper ? 8U : 0U;
		test::require(std::memcmp(cpu.vectors[31].bytes.data() + offset,
			memory.payload.data(), 8U) == 0);
		test::require(std::memcmp(cpu.vectors[31].bytes.data() + (8U - offset),
			before.bytes.data() + (8U - offset), 8U) == 0);

		memory = {};
		cpu = {};
		test::initialize(cpu);
		cpu.registers[2] = test::base;
		const auto source = cpu.vectors[3];
		test::require(arm64::Dispatcher{}.execute(cpu, memory,
			test::lane(false, upper, 2, 3)).reason ==
			arm64::StopReason::continued);
		test::require(std::memcmp(memory.written.data(),
			source.bytes.data() + offset, 8U) == 0);
	}
}

void faults_and_alignment_never_partially_commit()
{
	for (const auto &variant : test::variants) {
		test::common::ProbeMemory memory;
		memory.fault_read = true;
		memory.fault_write = true;
		arm64::CpuState cpu{};
		test::initialize(cpu);
		cpu.registers[2] = test::base;
		const auto before = cpu;
		const auto result = arm64::Dispatcher{}.execute(cpu, memory,
			test::unscaled(variant.size_code, variant.opc, 0, 2, 3));
		test::require(result.reason == arm64::StopReason::memory_fault);
		test::require(result.write == !variant.load);
		test::require_state_unchanged(cpu, before);

		if (variant.size > 1U) {
			memory = {};
			cpu = before;
			cpu.registers[2] = test::base + 1U;
			const auto misaligned_before = cpu;
			const auto misaligned = arm64::Dispatcher{}.execute(cpu, memory,
				test::unscaled(variant.size_code, variant.opc, 0, 2, 3));
			test::require(misaligned.reason ==
				arm64::StopReason::memory_fault);
			test::require(memory.reads == 0U && memory.writes == 0U);
			test::require_state_unchanged(cpu, misaligned_before);
		}
	}

	for (const bool load : {false, true}) {
		test::common::ProbeMemory memory;
		arm64::CpuState cpu{};
		test::initialize(cpu);
		cpu.registers[2] = test::base + 1U;
		const auto before = cpu;
		const auto result = arm64::Dispatcher{}.execute(cpu, memory,
			test::lane(load, true, 2, 3));
		test::require(result.reason == arm64::StopReason::memory_fault);
		test::require(memory.reads == 0U && memory.writes == 0U);
		test::require_state_unchanged(cpu, before);
	}
}

} // namespace

int main()
{
	unscaled_load_store_shapes_and_offsets();
	lane_loads_preserve_the_other_half_and_stores_select_the_lane();
	faults_and_alignment_never_partially_commit();
}
