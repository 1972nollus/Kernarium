// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_floating_test_support.hpp"

namespace fp_atomic = arm64_atomic_floating_test;
namespace test = arm64_load_store_test;

int main()
{
	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		const auto size = fp_atomic::size_bytes(size_code);
		const auto encoded = fp_atomic::instruction(size_code,
			fp_atomic::Operation::add, true, true, false, 2, 3, 3);
		fp_atomic::atomic::AtomicMemory memory;
		fp_atomic::arm64::CpuState cpu{};
		cpu.pc = fp_atomic::pc;
		cpu.registers[2] = fp_atomic::interpreter_address + 1;
		cpu.fpcr = 2U | (1U << 24) | 0x9f00U;
		cpu.fpsr = 0x0800'009fU;
		cpu.exclusive_address = 0x1234;
		cpu.exclusive_version = 99;
		fp_atomic::set_vector(cpu, 3, size_code == 0 ? 0x3f80 :
			size_code == 1 ? 0x3c00 : size_code == 2 ? 0x3f80'0000 :
			0x3ff0'0000'0000'0000ULL, size, std::byte{0xa5});
		const auto before = cpu;
		const auto result = fp_atomic::arm64::Dispatcher{}.execute(
			cpu, memory, encoded);
		test::require(result.reason == fp_atomic::arm64::StopReason::memory_fault &&
			result.address == fp_atomic::interpreter_address + 1 && result.write &&
			cpu.pc == before.pc && cpu.fpcr == before.fpcr &&
			cpu.fpsr == before.fpsr && cpu.vectors[3].bytes ==
			before.vectors[3].bytes && cpu.exclusive_address ==
			before.exclusive_address && cpu.exclusive_version ==
			before.exclusive_version && memory.atomic_pointers == 0 &&
			memory.reads == 0 && memory.writes == 0);
	}

	for (const bool fail_after_read : {false, true}) {
		fp_atomic::atomic::AtomicMemory memory;
		memory.direct = false;
		memory.fault = !fail_after_read;
		memory.fault_write = fail_after_read;
		memory.put(fp_atomic::interpreter_address, 0x3f80'0000, 4);
		fp_atomic::arm64::CpuState cpu{};
		cpu.pc = fp_atomic::pc;
		cpu.registers[2] = fp_atomic::interpreter_address;
		cpu.fpcr = 2U | 1U | 0x9f00U;
		cpu.fpsr = 0x0800'009fU;
		fp_atomic::set_vector(cpu, 3, 0x4000'0000, 4, std::byte{0xa5});
		const auto before = cpu;
		std::uint64_t host_fpcr{};
		std::uint64_t host_fpsr{};
		std::uint64_t after_fpcr{};
		std::uint64_t after_fpsr{};
		__asm__ volatile("mrs %0, fpcr\n\tmrs %1, fpsr" :
			"=r"(host_fpcr), "=r"(host_fpsr));
		const auto result = fp_atomic::arm64::Dispatcher{}.execute(cpu, memory,
			fp_atomic::instruction(2, fp_atomic::Operation::add, true, true,
				false, 2, 3, 3));
		__asm__ volatile("mrs %0, fpcr\n\tmrs %1, fpsr" :
			"=r"(after_fpcr), "=r"(after_fpsr));
		test::require(result.reason == fp_atomic::arm64::StopReason::memory_fault &&
			cpu.pc == before.pc && cpu.fpcr == before.fpcr &&
			cpu.fpsr == before.fpsr && cpu.vectors[3].bytes ==
			before.vectors[3].bytes && memory.get(
				fp_atomic::interpreter_address, 4) == 0x3f80'0000 &&
			host_fpcr == after_fpcr && host_fpsr == after_fpsr);
	}

	test::StaticMapping mapping{0x540'0000, 203,
		"/private/tmp/uml-atomic-floating-fault.XXXXXX"};
	const auto address = mapping.guest() + 4 * test::StaticMapping::page_size;
	for (unsigned size_code = 0; size_code < 4; ++size_code) {
		fp_atomic::arm64::CpuState cpu{};
		cpu.registers[2] = address + 1;
		fp_atomic::set_vector(cpu, 3, 0x4000'0000, 8);
		const auto before = cpu;
		const auto event = mapping.execute(fp_atomic::pc,
			fp_atomic::instruction(size_code, fp_atomic::Operation::add,
				true, true, false), cpu);
		test::require(event == fp_atomic::gadgets::Event::memory_fault &&
			cpu.pc == fp_atomic::pc && cpu.vectors[3].bytes ==
			before.vectors[3].bytes);
	}
	fp_atomic::arm64::CpuState cpu{};
	cpu.registers[2] = mapping.guest() + test::StaticMapping::length;
	fp_atomic::set_vector(cpu, 3, 0x4000'0000, 8);
	test::require(mapping.execute(fp_atomic::pc,
		fp_atomic::instruction(3, fp_atomic::Operation::add, false, false,
			false), cpu) == fp_atomic::gadgets::Event::memory_fault);
}
