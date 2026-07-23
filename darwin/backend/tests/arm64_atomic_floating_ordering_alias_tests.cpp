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

	const auto expected = [&](unsigned size, fp_atomic::Operation operation) {
		return operation == fp_atomic::Operation::add ? sum[size] :
			operation == fp_atomic::Operation::maximum || operation ==
				fp_atomic::Operation::maximum_number ? operand[size] :
				observed[size];
	};

	for (const bool direct : {false, true}) {
		for (unsigned size_code = 0; size_code < 4; ++size_code) {
			const auto size = fp_atomic::size_bytes(size_code);
			for (const auto operation : fp_atomic::operations) {
				for (const bool acquire : {false, true}) {
					for (const bool release : {false, true}) {
						/* Rs==Rt must capture the source before returning memory. */
						fp_atomic::atomic::AtomicMemory memory;
						fp_atomic::arm64::CpuState cpu{};
						cpu.fpsr = 0x91;
						const auto encoded = fp_atomic::instruction(size_code,
							operation, acquire, release, false, 2, 3, 3);
						const auto result = fp_atomic::execute_interpreter(cpu,
							memory, encoded, observed[size_code], operand[size_code],
							direct);
						test::require(result.reason == fp_atomic::arm64::
							StopReason::continued && memory.get(
								fp_atomic::interpreter_address, size) ==
								expected(size_code, operation) &&
							fp_atomic::vector_bits(cpu, 3, size) ==
								observed[size_code] &&
							fp_atomic::vector_upper_zero(cpu, 3, size) &&
							cpu.fpsr == 0x91);

						/* V31 is an ordinary SIMD source and destination. */
						memory = {};
						cpu = {};
						const auto v31 = fp_atomic::instruction(size_code,
							operation, acquire, release, false, 2, 3, 31);
						fp_atomic::execute_interpreter(cpu, memory, v31,
							observed[size_code], operand[size_code], direct);
						test::require(fp_atomic::vector_bits(cpu, 31, size) ==
							observed[size_code] &&
							fp_atomic::vector_upper_zero(cpu, 31, size));
					}
				}

				for (const bool release : {false, true}) {
					/* SP addressing is selected by Rn==31. */
					fp_atomic::atomic::AtomicMemory memory;
					fp_atomic::arm64::CpuState cpu{};
					const auto encoded = fp_atomic::instruction(size_code,
						operation, false, release, false, 31, 3, 4);
					const auto result = fp_atomic::execute_interpreter(cpu,
						memory, encoded, observed[size_code], operand[size_code],
						direct, 31, 3);
					test::require(result.reason == fp_atomic::arm64::
						StopReason::continued && cpu.sp ==
						fp_atomic::interpreter_address);

					/* Store-only preserves every SIMD register, including V31
					 * when it is simultaneously Rs and the encoded Rt field. */
					memory = {};
					cpu = {};
					fp_atomic::set_vector(cpu, 31, operand[size_code], size,
						std::byte{0xa5});
					const auto before = cpu.vectors[31];
					const auto store = fp_atomic::instruction(size_code,
						operation, false, release, true, 2, 31, 31);
					const auto stored = fp_atomic::execute_interpreter(cpu,
						memory, store, observed[size_code], operand[size_code],
						direct, 2, 31);
					test::require(stored.reason == fp_atomic::arm64::
						StopReason::continued && cpu.vectors[31].bytes ==
						before.bytes && memory.get(fp_atomic::interpreter_address,
							size) == expected(size_code, operation));
				}
			}
		}
	}
}
