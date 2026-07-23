// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_floating_test_support.hpp"

namespace fp_atomic = arm64_atomic_floating_test;
namespace test = arm64_load_store_test;

int main()
{
	constexpr std::array observed{0x3f80ULL, 0x3c00ULL,
		0x3f80'0000ULL, 0x3ff0'0000'0000'0000ULL};
	constexpr std::array operand{0x4000ULL, 0x4000ULL,
		0x4000'0000ULL, 0x4000'0000'0000'0000ULL};
	constexpr std::array sum{0x4040ULL, 0x4200ULL,
		0x4040'0000ULL, 0x4008'0000'0000'0000ULL};

	for (const bool direct : {false, true}) {
		for (unsigned size_code = 0; size_code < 4; ++size_code) {
			const auto size = fp_atomic::size_bytes(size_code);
			for (const auto operation : fp_atomic::operations) {
				for (const bool store_only : {false, true}) {
					for (const bool acquire : {false, true}) {
						if (store_only && acquire)
							continue;
						for (const bool release : {false, true}) {
							const auto encoded = fp_atomic::instruction(size_code,
								operation, acquire, release, store_only);
							const auto decoded = fp_atomic::detail::
								decode_atomic_floating(encoded);
							test::require(decoded.recognized && decoded.valid &&
								decoded.bfloat == (size_code == 0U) &&
								decoded.return_old == !store_only &&
								decoded.acquire == acquire &&
								decoded.release == release &&
								decoded.width == size * 8U &&
								static_cast<unsigned>(decoded.operation) ==
									static_cast<unsigned>(operation));

							fp_atomic::atomic::AtomicMemory memory;
							fp_atomic::arm64::CpuState cpu{};
							cpu.fpcr = 0;
							cpu.fpsr = 0x0800'009fU;
							fp_atomic::set_vector(cpu, 4, 0xdead'beefULL,
								size, std::byte{0x5a});
							const auto before_destination = cpu.vectors[4];
							const auto result = fp_atomic::execute_interpreter(cpu,
								memory, encoded, observed[size_code],
								operand[size_code], direct);
							test::require(result.reason == fp_atomic::arm64::
								StopReason::continued && cpu.pc == fp_atomic::pc + 4 &&
								cpu.fpsr == 0x0800'009fU);
							const auto expected = operation == fp_atomic::Operation::add ?
								sum[size_code] : operation ==
									fp_atomic::Operation::maximum || operation ==
									fp_atomic::Operation::maximum_number ?
								operand[size_code] : observed[size_code];
							test::require(memory.get(fp_atomic::interpreter_address,
								size) == expected);
							if (store_only) {
								test::require(cpu.vectors[4].bytes ==
									before_destination.bytes);
							} else {
								test::require(fp_atomic::vector_bits(cpu, 4, size) ==
									observed[size_code] &&
									fp_atomic::vector_upper_zero(cpu, 4, size));
							}
						}
					}
				}
			}
		}
	}

	const std::array invalid{
		fp_atomic::instruction_raw(0, false, false, false, 1, 2, 3, 4),
		fp_atomic::instruction_raw(1, false, false, false, 2, 2, 3, 4),
		fp_atomic::instruction_raw(3, false, false, false, 3, 2, 3, 4),
		fp_atomic::instruction_raw(2, true, false, true, 0, 2, 3, 31),
		fp_atomic::instruction_raw(2, false, false, true, 0, 2, 3, 4),
	};
	test::StaticMapping mapping{0x520'0000, 201,
		"/private/tmp/uml-atomic-floating-allocation.XXXXXX"};
	for (const auto encoded : invalid) {
		const auto decoded = fp_atomic::detail::decode_atomic_floating(encoded);
		test::require(decoded.recognized && !decoded.valid);
		fp_atomic::atomic::AtomicMemory memory;
		fp_atomic::arm64::CpuState cpu{};
		cpu.pc = fp_atomic::pc;
		cpu.registers[2] = fp_atomic::interpreter_address + 1;
		cpu.fp_loaded = false;
		const auto before = cpu;
		const auto result = fp_atomic::arm64::Dispatcher{}.execute(
			cpu, memory, encoded);
		test::require(result.reason == fp_atomic::arm64::StopReason::
			illegal_instruction && cpu.pc == before.pc && !cpu.fp_loaded &&
			memory.atomic_pointers == 0 && memory.reads == 0 &&
			memory.writes == 0);
		fp_atomic::arm64::CpuState gadget_cpu{};
		test::require(mapping.execute(fp_atomic::pc, encoded, gadget_cpu) ==
			fp_atomic::gadgets::Event::illegal_instruction);
	}
}
