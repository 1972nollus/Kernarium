// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_ordered_test_support.hpp"

#include <cstring>

namespace test = arm64_simd_ordered_test;
namespace arm64 = uml::backend::arm64;

namespace {

void every_gadget_shape_uses_gpr_and_sp_addressing()
{
	test::common::StaticMapping mapping{0xc0'0000, 79,
		"/private/tmp/uml-simd-ordered.XXXXXX"};
	const auto target = mapping.guest() +
		4U * test::common::StaticMapping::page_size;
	for (const auto &variant : test::variants)
		for (const bool sp_base : {false, true})
			for (const unsigned imm9 : {0U, 255U, 256U, 511U}) {
				arm64::CpuState cpu{};
				test::initialize(cpu);
				const auto offset = test::common::signed_imm9(imm9);
				const auto adjusted = target -
					static_cast<std::uint64_t>(offset);
				if (sp_base)
					cpu.sp = adjusted;
				else
					cpu.registers[2] = adjusted;
				const auto source = cpu.vectors[3];
				test::common::ProbeMemory payload;
				std::memcpy(mapping.at(target), payload.payload.data(),
					variant.size);
				const auto instruction = test::unscaled(variant.size_code,
					variant.opc, imm9, sp_base ? 31U : 2U, 3U);
				test::require(mapping.execute(test::pc, instruction, cpu) ==
					test::gadgets::Event::system_call);
				if (variant.load) {
					for (std::size_t index = 0; index < 16U; ++index)
						test::require(cpu.vectors[3].bytes[index] ==
							(index < variant.size ? payload.payload[index] :
							 std::byte{}));
				} else {
					test::require(std::memcmp(mapping.at(target),
						source.bytes.data(), variant.size) == 0);
				}
			}
}

void lane_gadgets_preserve_and_select_exact_halves()
{
	test::common::StaticMapping mapping{0xd0'0000, 83,
		"/private/tmp/uml-simd-ordered-lane.XXXXXX"};
	const auto target = mapping.guest() +
		3U * test::common::StaticMapping::page_size;
	for (const bool load : {false, true})
		for (const bool upper : {false, true})
			for (const bool sp_base : {false, true}) {
				arm64::CpuState cpu{};
				test::initialize(cpu);
				if (sp_base)
					cpu.sp = target;
				else
					cpu.registers[2] = target;
				const auto before = cpu.vectors[3];
				test::common::ProbeMemory payload;
				std::memcpy(mapping.at(target), payload.payload.data(), 8U);
				test::require(mapping.execute(test::pc,
					test::lane(load, upper, sp_base ? 31U : 2U, 3U), cpu) ==
					test::gadgets::Event::system_call);
				const auto offset = upper ? 8U : 0U;
				if (load) {
					test::require(std::memcmp(
						cpu.vectors[3].bytes.data() + offset,
						payload.payload.data(), 8U) == 0);
					test::require(std::memcmp(
						cpu.vectors[3].bytes.data() + (8U - offset),
						before.bytes.data() + (8U - offset), 8U) == 0);
				} else {
					test::require(std::memcmp(mapping.at(target),
						before.bytes.data() + offset, 8U) == 0);
				}
			}
}

void gadget_alignment_faults_before_memory_access()
{
	test::common::StaticMapping mapping{0xe0'0000, 89,
		"/private/tmp/uml-simd-ordered-fault.XXXXXX"};
	arm64::CpuState cpu{};
	test::initialize(cpu);
	cpu.registers[2] = mapping.guest() + 1U;
	const auto before = cpu;
	test::require(mapping.execute(test::pc,
		test::unscaled(0, 3, 0, 2, 3), cpu) ==
		test::gadgets::Event::memory_fault);
	test::require_vectors_equal(cpu, before);
}

} // namespace

int main()
{
	every_gadget_shape_uses_gpr_and_sp_addressing();
	lane_gadgets_preserve_and_select_exact_halves();
	gadget_alignment_faults_before_memory_access();
}
