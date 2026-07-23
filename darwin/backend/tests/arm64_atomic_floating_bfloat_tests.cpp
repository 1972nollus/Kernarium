// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_floating_test_support.hpp"

namespace fp_atomic = arm64_atomic_floating_test;
namespace test = arm64_load_store_test;

struct Case final {
	fp_atomic::Operation operation;
	std::uint16_t memory;
	std::uint16_t operand;
	std::uint32_t fpcr;
	std::uint16_t expected;
};

int main()
{
	constexpr std::array cases{
		Case{fp_atomic::Operation::add, 0x3f80, 0x4000, 0, 0x4040},
		Case{fp_atomic::Operation::add, 0x3f80, 0x3b80, 0, 0x3f80},
		Case{fp_atomic::Operation::add, 0x3f80, 0x3b80, 1U << 22, 0x3f81},
		Case{fp_atomic::Operation::add, 0xbf80, 0xbb80, 2U << 22, 0xbf81},
		Case{fp_atomic::Operation::add, 0xbf80, 0xbb80, 1U << 22, 0xbf80},
		Case{fp_atomic::Operation::add, 0x0001, 0x0000, 0, 0x0001},
		Case{fp_atomic::Operation::add, 0x0001, 0x0000, 1U << 24, 0x0000},
		Case{fp_atomic::Operation::add, 0x0001, 0x0000, 1U, 0x0000},
		Case{fp_atomic::Operation::add, 0x7f80, 0xff80, 0, 0x7fc0},
		Case{fp_atomic::Operation::add, 0x7f7f, 0x7f7f, 0, 0x7f80},
		Case{fp_atomic::Operation::add, 0x7f7f, 0x7f7f, 3U << 22, 0x7f7f},
		Case{fp_atomic::Operation::add, 0x7fa1, 0x3f80, 0, 0x7fc0},
		Case{fp_atomic::Operation::maximum, 0x8000, 0x0000, 0, 0x0000},
		Case{fp_atomic::Operation::minimum, 0x8000, 0x0000, 0, 0x8000},
		Case{fp_atomic::Operation::maximum, 0x7fc5, 0x3f80, 0, 0x7fc0},
		Case{fp_atomic::Operation::maximum, 0x7fc5, 0x3f80, 2U, 0x7fc0},
		Case{fp_atomic::Operation::minimum, 0x3f80, 0xffc5, 1U << 25,
			0x7fc0},
		Case{fp_atomic::Operation::maximum_number, 0x7fc5, 0x3f80, 0,
			0x3f80},
		Case{fp_atomic::Operation::maximum_number, 0x3f80, 0x7fc5, 0,
			0x3f80},
		Case{fp_atomic::Operation::minimum_number, 0x7fc5, 0xbf80, 0,
			0xbf80},
		Case{fp_atomic::Operation::minimum_number, 0xbf80, 0x7fc5, 0,
			0xbf80},
		Case{fp_atomic::Operation::maximum_number, 0x7f81, 0x3f80, 0,
			0x7fc0},
		Case{fp_atomic::Operation::minimum_number, 0x7fc1, 0xffc1, 0,
			0x7fc0},
		Case{fp_atomic::Operation::maximum, 0x0001, 0x8000, 1U, 0x0000},
		Case{fp_atomic::Operation::minimum, 0x8001, 0x0000, 1U << 24,
			0x8000},
	};

	for (const bool direct : {false, true}) {
		for (const auto &item : cases) {
			fp_atomic::atomic::AtomicMemory memory;
			fp_atomic::arm64::CpuState cpu{};
			cpu.fpcr = item.fpcr;
			cpu.fpsr = 0x0800'009fU;
			const auto encoded = fp_atomic::instruction(0, item.operation,
				false, false, false);
			const auto result = fp_atomic::execute_interpreter(cpu, memory,
				encoded, item.memory, item.operand, direct);
			test::require(result.reason ==
				fp_atomic::arm64::StopReason::continued &&
				memory.get(fp_atomic::interpreter_address, 2) == item.expected &&
				fp_atomic::vector_bits(cpu, 4, 2) == item.memory &&
				fp_atomic::vector_upper_zero(cpu, 4, 2) &&
				fp_atomic::vector_bits(cpu, 3, 2) == item.operand &&
				cpu.fpcr == item.fpcr && cpu.fpsr == 0x0800'009fU);
		}
	}
}
