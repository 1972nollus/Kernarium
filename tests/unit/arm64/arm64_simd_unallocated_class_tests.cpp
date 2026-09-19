// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

namespace {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

struct UnallocatedWord final {
	std::uint32_t instruction;
	const char *reason;
};

/*
 * Every unallocated row of the A64 "Data Processing -- Scalar Floating-Point
 * and Advanced SIMD" class table, named by its op0 (31:28), op1 (24:23),
 * op2 (22:19) and op3 (18:10) columns. Each row borders allocated classes, so
 * a decoder that ignores one class-selecting bit executes it as a neighbour.
 */
constexpr std::array class_rows{
	UnallocatedWord{0x8e00'0023U, "op0 10x0"},
	UnallocatedWord{0xde00'0023U, "op0 11x1"},
	UnallocatedWord{0xee00'0023U, "op0 1110"},
	UnallocatedWord{0x4f88'1423U, "op0 01xx, op1 11, op3 xxxxxxxx1"},
	UnallocatedWord{0x0f88'1423U, "op0 00x0, op1 11, op3 xxxxxxxx1"},
	UnallocatedWord{0xcf00'0023U, "op0 1100, op1 1x"},
	UnallocatedWord{0x0e00'8023U,
		"op0 0xx0, op1 0x, op2 x0xx, op3 xxx1xxxx0"},
	UnallocatedWord{0x5e00'8023U,
		"op0 0101, op1 0x, op2 x0xx, op3 xxx1xxxx0"},
	UnallocatedWord{0x5e00'0823U,
		"op0 0101, op1 0x, op2 x0xx, op3 xxx0xxx10"},
	UnallocatedWord{0x7e00'0023U,
		"op0 0111, op1 0x, op2 x0xx, op3 xxxxxxxx0"},
	UnallocatedWord{0x0e24'0823U,
		"op0 0xx0, op1 0x, op2 x1xx, op3 1xxxxxx10"},
	UnallocatedWord{0x0e22'0823U,
		"op0 0xx0, op1 0x, op2 x1xx, op3 01xxxxx10"},
	UnallocatedWord{0x5e24'3823U,
		"op0 01x1, op1 0x, op2 x1xx, op3 1xxxxxx10"},
	UnallocatedWord{0x5e22'3823U,
		"op0 01x1, op1 0x, op2 x1xx, op3 01xxxxx10"},
	UnallocatedWord{0xcea0'0023U, "op0 1100, op1 01, op2 x1xx"},
	UnallocatedWord{0xced0'0023U, "op0 1100, op1 01, op2 101x"},
	UnallocatedWord{0xcec8'0023U, "op0 1100, op1 01, op2 1001"},
	UnallocatedWord{0xcec0'0023U,
		"op0 1100, op1 01, op2 1000, op3 other than 0001000xx"},
	UnallocatedWord{0x0e81'0423U,
		"op0 0xx0, op1 01, op2 00xx, op3 xxx0xxxx1"},
	UnallocatedWord{0x5e81'0423U,
		"op0 01x1, op1 01, op2 00xx, op3 xxx0xxxx1"},
	UnallocatedWord{0x0e40'4423U,
		"op0 0xx0, op1 0x, op2 10xx, op3 xxx01xxx1"},
	UnallocatedWord{0x5e40'4423U,
		"op0 01x1, op1 0x, op2 10xx, op3 xxx01xxx1"},
	UnallocatedWord{0xce00'8023U,
		"op0 1100, op1 00, op2 0xxx, op3 xxx1xxxxx"},
	UnallocatedWord{0xce40'c023U,
		"op0 1100, op1 00, op2 10xx, op3 xxx11xxxx"},
	UnallocatedWord{0xce60'a023U,
		"op0 1100, op1 00, op2 11xx, op3 xxx1x1xxx"},
	UnallocatedWord{0xce60'9023U,
		"op0 1100, op1 00, op2 11xx, op3 xxx1x01xx"},
	UnallocatedWord{0x1e20'8023U,
		"op0 x0x1, op1 0x, op2 x1xx, op3 xxx100000"},
	UnallocatedWord{0x0e28'0823U,
		"op0 00x0, op1 0x, op2 x101, op3 00xxxxx10"},
	UnallocatedWord{0x6e28'0823U,
		"op0 011x, op1 0x, op2 x101, op3 00xxxxx10"},
	UnallocatedWord{0x0e38'0823U,
		"op0 0xx0, op1 0x, op2 0111, op3 00xxxxx10"},
	UnallocatedWord{0x5e38'3823U,
		"op0 01x1, op1 0x, op2 0111, op3 00xxxxx10"},
};

/*
 * Bits 20:17 must be 0000 for two-register miscellaneous and 1000 for
 * across-lanes words. These share every other class bit with those classes.
 */
constexpr std::array class_selector_words{
	UnallocatedWord{0x0e22'2823U, "bits 20:17 0001, 8B"},
	UnallocatedWord{0x0e62'2823U, "bits 20:17 0001, 4H"},
	UnallocatedWord{0x0ea2'2823U, "bits 20:17 0001, 2S"},
	UnallocatedWord{0x4e22'2823U, "bits 20:17 0001, 16B"},
	UnallocatedWord{0x0e32'3823U, "bits 20:17 1001, 8B"},
	UnallocatedWord{0x0e32'a823U, "bits 20:17 1001, 8B, opcode 01010"},
	UnallocatedWord{0x2e32'3823U, "bits 20:17 1001, U=1"},
	UnallocatedWord{0x4eb2'3823U, "bits 20:17 1001, 4S"},
};

/* Unallocated rows inside allocated classes. */
constexpr std::array class_opcodes{
	UnallocatedWord{0x0e21'0823U,
		"two-register miscellaneous opcode 10000"},
	UnallocatedWord{0x0e30'0823U, "across lanes opcode 00000"},
	UnallocatedWord{0x0e30'2823U, "across lanes opcode 00010"},
	UnallocatedWord{0x0e78'0823U,
		"FP16 two-register miscellaneous opcode 00000"},
	UnallocatedWord{0x0e78'2823U,
		"FP16 two-register miscellaneous opcode 00010"},
	UnallocatedWord{0x0e78'3823U,
		"FP16 two-register miscellaneous opcode 00011"},
	UnallocatedWord{0x5e78'3823U,
		"scalar FP16 two-register miscellaneous opcode 00011"},
	UnallocatedWord{0x5e78'c823U,
		"scalar FP16 two-register miscellaneous a=0 opcode 01100"},
	UnallocatedWord{0x5e30'3823U, "scalar pairwise opcode 00011"},
	UnallocatedWord{0x5ea0'dc23U,
		"scalar three same FMULX opcode with size 1x"},
	UnallocatedWord{0x5ec0'1c23U,
		"scalar FP16 three same FMULX opcode with a=1"},
	UnallocatedWord{0x5e41'0423U,
		"scalar FP16 three same a=0 U=0 opcode 000"},
	UnallocatedWord{0x4e28'b823U, "AES opcode 01011"},
	UnallocatedWord{0x4e29'8823U, "AES opcode 11000"},
	UnallocatedWord{0x5e28'4823U, "SHA two-register opcode 00100"},
	UnallocatedWord{0x5e29'8823U, "SHA two-register opcode 11000"},
	UnallocatedWord{0x3ee0'8423U, "FP conditional compare with S=1"},
	UnallocatedWord{0x3ee0'8823U,
		"FP data-processing (2 source) with S=1, opcode 1000"},
	UnallocatedWord{0x3ee0'9823U,
		"FP data-processing (2 source) with S=1, opcode 1001"},
	UnallocatedWord{0x3e21'd823U,
		"FP data-processing (2 source) with S=1, opcode 1101"},
	UnallocatedWord{0x1e21'd823U,
		"FP data-processing (2 source) opcode 1101"},
	UnallocatedWord{0x1ea1'b823U,
		"FP data-processing (2 source) ftype 10, opcode 1011"},
	UnallocatedWord{0x5e20'1c23U, "scalar three same opcode 00011, U=0"},
	UnallocatedWord{0x7e20'1c23U, "scalar three same opcode 00011, U=1"},
	UnallocatedWord{0x5e20'c423U, "scalar three same FMAXNM opcode"},
	UnallocatedWord{0x5e20'cc23U, "scalar three same FMLA opcode"},
	UnallocatedWord{0x5e20'd423U, "scalar three same FADD opcode"},
	UnallocatedWord{0x5e20'f423U, "scalar three same FMAX opcode"},
	UnallocatedWord{0x5ea0'e423U,
		"scalar three same FCMEQ opcode with a=1"},
	UnallocatedWord{0x7e20'c423U, "scalar three same FMAXNMP opcode"},
	UnallocatedWord{0x7e20'dc23U, "scalar three same FMUL opcode"},
	UnallocatedWord{0x7e20'f423U, "scalar three same FMAXP opcode"},
	UnallocatedWord{0x7e20'fc23U, "scalar three same FDIV opcode"},
	UnallocatedWord{0x0ea0'e423U, "three same FCMEQ opcode with a=1, 2S"},
	UnallocatedWord{0x4ee0'e423U, "three same FCMEQ opcode with a=1, 2D"},
	UnallocatedWord{0x2ea0'e823U,
		"two-register miscellaneous FCMLT (zero) opcode with U=1"},
	UnallocatedWord{0x2ea1'8823U,
		"two-register miscellaneous FRINTA opcode with a=1"},
	UnallocatedWord{0x5e21'8823U,
		"scalar two-register miscellaneous FRINTN opcode"},
	UnallocatedWord{0x7e21'9823U,
		"scalar two-register miscellaneous FRINTX opcode"},
	UnallocatedWord{0x7ea0'e823U,
		"scalar two-register miscellaneous FCMLT opcode with U=1"},
	UnallocatedWord{0x5eb0'd823U,
		"scalar pairwise FADDP opcode with a=1, U=0"},
	UnallocatedWord{0x7eb0'd823U,
		"scalar pairwise FADDP opcode with a=1, U=1"},
	UnallocatedWord{0x1e04'8023U,
		"conversion between FP and fixed-point opcode 100"},
	UnallocatedWord{0x9e45'4023U,
		"conversion between FP and fixed-point opcode 101, 64-bit"},
};

[[nodiscard]] arm64::CpuState state(unsigned seed)
{
	arm64::CpuState cpu{};
	for (unsigned reg = 0; reg < cpu.vectors.size(); ++reg)
		for (unsigned byte = 0; byte < cpu.vectors[reg].bytes.size(); ++byte)
			cpu.vectors[reg].bytes[byte] = static_cast<std::byte>(
				seed * 67U + reg * 29U + byte * 17U + 3U);
	for (unsigned reg = 0; reg < cpu.registers.size(); ++reg)
		cpu.registers[reg] = 0x1020'3040'5060'7080ULL ^
			(static_cast<std::uint64_t>(reg + seed) *
			 0x0101'0101'0101'0101ULL);
	cpu.sp = 0xfeed'face'cafe'bee0ULL;
	cpu.nzcv = 0x6000'0000U;
	cpu.fpcr = 0x0040'0000U;
	cpu.fpsr = seed & 1U;
	cpu.fpmr = 0x0123'4567'89ab'cdefULL;
	cpu.tls = 0xfedc'ba98'7654'3210ULL;
	return cpu;
}

/*
 * No row above depends on bits 9:0, so each word is also checked with other
 * values there.
 */
void check_words(engine_tests::EngineHarness &harness, const char *group,
	std::span<const UnallocatedWord> words, unsigned &seed)
{
	for (const auto &word : words) {
		for (const auto low : {word.instruction & 0x3ffU, 0x000U, 0x3ffU,
			     0x0e7U}) {
			const auto instruction = (word.instruction & ~0x3ffU) | low;
			char name[160];
			std::snprintf(name, sizeof(name),
				"simd-unallocated-class %s %08x (%s)", group,
				instruction, word.reason);
			harness.check_illegal(name, instruction, state(++seed));
		}
	}
}

} // namespace

int main()
{
	engine_tests::EngineHarness harness;
	unsigned seed = 0;
	check_words(harness, "class-row", class_rows, seed);
	check_words(harness, "bits-20-17", class_selector_words, seed);
	check_words(harness, "class-opcode", class_opcodes, seed);
	harness.print_summary("arm64-simd-unallocated-class");
	return 0;
}
