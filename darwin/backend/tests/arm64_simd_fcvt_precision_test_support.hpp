// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_simd_fcmp_test_support.hpp"

namespace arm64_simd_fcvt_precision_test {

namespace base = arm64_simd_test;
namespace move = arm64_simd_fmov_test;

[[nodiscard]] constexpr std::uint32_t instruction(unsigned source_width,
	unsigned destination_width, unsigned destination, unsigned source)
{
	return 0x1e22'4000U | (move::type_for_bytes(source_width / 8U) << 22) |
		(move::type_for_bytes(destination_width / 8U) << 15) |
		((source & 31U) << 5) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint64_t one(unsigned width)
{
	return width == 16U ? 0x3c00U : width == 32U ? 0x3f80'0000U :
		0x3ff0'0000'0000'0000ULL;
}

[[nodiscard]] constexpr std::uint64_t infinity(unsigned width,
	bool negative = false)
{
	const auto value = width == 16U ? 0x7c00U :
		width == 32U ? 0x7f80'0000U : 0x7ff0'0000'0000'0000ULL;
	return value | (negative ? std::uint64_t{1} << (width - 1U) : 0U);
}

[[nodiscard]] constexpr std::uint64_t quiet_nan(unsigned width,
	bool negative = false)
{
	const auto value = width == 16U ? 0x7e55U :
		width == 32U ? 0x7fc1'2345U : 0x7ff8'1234'5678'9abcULL;
	return value | (negative ? std::uint64_t{1} << (width - 1U) : 0U);
}

[[nodiscard]] constexpr std::uint64_t signaling_nan(unsigned width,
	bool negative = false)
{
	const auto value = width == 16U ? 0x7d51U :
		width == 32U ? 0x7f81'2345U : 0x7ff0'1234'5678'9abcULL;
	return value | (negative ? std::uint64_t{1} << (width - 1U) : 0U);
}

inline void require_result(std::uint32_t encoded, unsigned destination_width,
	unsigned destination, std::uint64_t bits, std::uint32_t exceptions,
	const uml::backend::arm64::CpuState &initial, base::Harness &harness)
{
	auto expected = initial;
	expected.pc = base::pc + 4U;
	auto destination_bytes = (initial.fpcr & 4U) != 0U ?
		initial.vectors[destination].bytes : std::array<std::byte, 16>{};
	move::write_bits(destination_bytes, 0U, destination_width / 8U, bits);
	expected.vectors[destination].bytes = destination_bytes;
	expected.fpsr |= exceptions;
	const auto interpreted = harness.run_interpreter(encoded, initial);
	if (expected.vectors[destination].bytes !=
	    interpreted.vectors[destination].bytes)
		std::fprintf(stderr,
			"FCVT interpreter vector mismatch insn=%08x src=%016llx expected=%016llx actual=%016llx fpcr=%08x\n",
			encoded,
			static_cast<unsigned long long>(move::read_bits(
				initial.vectors[(encoded >> 5) & 31U].bytes, 0U,
				((encoded >> 22) & 3U) == 3U ? 2U :
				 ((encoded >> 22) & 3U) == 0U ? 4U : 8U)),
			static_cast<unsigned long long>(move::read_bits(
				expected.vectors[destination].bytes, 0U, 8U)),
			static_cast<unsigned long long>(move::read_bits(
				interpreted.vectors[destination].bytes, 0U, 8U)),
			initial.fpcr);
	base::require_architecture_equal(expected, interpreted);
	harness.require_differential(encoded, initial);
}

inline void set_source(uml::backend::arm64::CpuState &cpu, unsigned source,
	unsigned source_width, std::uint64_t bits)
{
	move::write_bits(cpu.vectors[source].bytes, 0U, source_width / 8U, bits);
}

} // namespace arm64_simd_fcvt_precision_test
