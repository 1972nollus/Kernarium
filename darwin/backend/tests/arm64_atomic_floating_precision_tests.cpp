// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_atomic_floating_test_support.hpp"

namespace fp_atomic = arm64_atomic_floating_test;
namespace test = arm64_load_store_test;

struct Width final {
	unsigned size_code;
	std::uint64_t sign;
	std::uint64_t one;
	std::uint64_t two;
	std::uint64_t three;
	std::uint64_t half_ulp;
	std::uint64_t next;
	std::uint64_t quiet_nan;
	std::uint64_t signaling_nan;
	std::uint64_t default_nan;
	std::uint64_t positive_infinity;
};

int main()
{
	constexpr std::array widths{
		Width{1, 0x8000ULL, 0x3c00, 0x4000, 0x4200, 0x1000, 0x3c01,
			0x7e15, 0x7d15, 0x7e00, 0x7c00},
		Width{2, 0x8000'0000ULL, 0x3f80'0000, 0x4000'0000,
			0x4040'0000, 0x3380'0000, 0x3f80'0001, 0x7fc1'2345,
			0x7f81'2345, 0x7fc0'0000, 0x7f80'0000},
		Width{3, 0x8000'0000'0000'0000ULL, 0x3ff0'0000'0000'0000,
			0x4000'0000'0000'0000, 0x4008'0000'0000'0000,
			0x3ca0'0000'0000'0000, 0x3ff0'0000'0000'0001,
			0x7ff8'1234'5678'9abc, 0x7ff0'1234'5678'9abc,
			0x7ff8'0000'0000'0000, 0x7ff0'0000'0000'0000},
	};

	const auto run = [](const Width &width, fp_atomic::Operation operation,
		std::uint64_t observed, std::uint64_t operand, std::uint32_t fpcr,
		std::uint64_t expected) {
		for (const bool direct : {false, true}) {
			fp_atomic::atomic::AtomicMemory memory;
			fp_atomic::arm64::CpuState cpu{};
			cpu.fpcr = fpcr;
			cpu.fpsr = 0x0800'009fU;
			const auto encoded = fp_atomic::instruction(width.size_code,
				operation, false, false, false);
			const auto result = fp_atomic::execute_interpreter(cpu, memory,
				encoded, observed, operand, direct);
			const auto size = fp_atomic::size_bytes(width.size_code);
			test::require(result.reason == fp_atomic::arm64::StopReason::continued &&
				memory.get(fp_atomic::interpreter_address, size) == expected &&
				fp_atomic::vector_bits(cpu, 4, size) == observed &&
				fp_atomic::vector_upper_zero(cpu, 4, size) &&
				cpu.fpcr == fpcr && cpu.fpsr == 0x0800'009fU);
		}
	};

	for (const auto &width : widths) {
		run(width, fp_atomic::Operation::add, width.one, width.two, 0,
			width.three);
		run(width, fp_atomic::Operation::add, width.one, width.half_ulp, 0,
			width.one);
		run(width, fp_atomic::Operation::add, width.one, width.half_ulp,
			1U << 22, width.next);
		run(width, fp_atomic::Operation::add, width.positive_infinity,
			width.positive_infinity | width.sign, 0, width.default_nan);
		run(width, fp_atomic::Operation::maximum, width.sign, 0, 0, 0);
		run(width, fp_atomic::Operation::minimum, width.sign, 0, 0,
			width.sign);
		run(width, fp_atomic::Operation::maximum, width.quiet_nan, width.one,
			2U | 0x0000'9f00U, width.default_nan);
		run(width, fp_atomic::Operation::minimum, width.one, width.quiet_nan,
			0, width.default_nan);
		run(width, fp_atomic::Operation::maximum_number, width.quiet_nan,
			width.one, 0, width.one);
		run(width, fp_atomic::Operation::minimum_number, width.one,
			width.quiet_nan, 0, width.one);
		run(width, fp_atomic::Operation::maximum_number, width.signaling_nan,
			width.one, 0, width.default_nan);
		run(width, fp_atomic::Operation::minimum_number, width.quiet_nan,
			width.quiet_nan | width.sign, 0, width.default_nan);

		const std::uint32_t flush = width.size_code == 1U ?
			1U << 19 : 1U;
		run(width, fp_atomic::Operation::add, 1, 0, flush, 0);
		if (width.size_code != 1U)
			run(width, fp_atomic::Operation::add, 1, 0, 1U << 24, 0);
	}
}
