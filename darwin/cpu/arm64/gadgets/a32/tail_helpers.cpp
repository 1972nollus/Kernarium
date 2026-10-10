// SPDX-License-Identifier: GPL-2.0-or-later
//
// The tail helper of the AArch32 gadget part: the rare integer forms whose
// gadget would be long and whose frequency does not justify one (the
// parallel add and subtract families, SEL, USAD8, the dual 16-bit
// multiplies, the 16-bit saturates and extends, and the saturating
// arithmetic of QADD and its relatives). The gadget passes CpuState and one
// operand word (a32_builder.hpp, tail::) and this function reads and writes
// the guest registers, GE and Q directly. The operation numbering is
// generate_a32.py's tail_ops().
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/gadgets/a32/a32_builder.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

using uml::cpu::arm64::CpuState;
namespace tail = uml::cpu::arm64::gadgets::a32::tail;

static_assert(offsetof(CpuState, registers) == 0);

std::uint32_t read(const CpuState &cpu, std::uint64_t word, unsigned shift)
{
	const auto offset = static_cast<std::size_t>(word >> shift & 0xffU);
	std::uint32_t value;
	std::memcpy(&value, reinterpret_cast<const std::byte *>(&cpu) + offset,
		    sizeof(value));
	return value;
}

void write(CpuState &cpu, std::uint64_t word, unsigned shift,
	   std::uint32_t value)
{
	const auto offset = static_cast<std::size_t>(word >> shift & 0xffU);
	const std::uint64_t slot = value;
	std::memcpy(reinterpret_cast<std::byte *>(&cpu) + offset, &slot,
		    sizeof(slot));
}

std::int32_t signed_saturate(std::int64_t value, unsigned width,
			     bool &saturated)
{
	const std::int64_t high = (std::int64_t{1} << (width - 1)) - 1;
	const std::int64_t low = -high - 1;
	if (value > high) {
		saturated = true;
		return static_cast<std::int32_t>(high);
	}
	if (value < low) {
		saturated = true;
		return static_cast<std::int32_t>(low);
	}
	return static_cast<std::int32_t>(value);
}

std::uint32_t unsigned_saturate(std::int64_t value, unsigned width,
				bool &saturated)
{
	const std::int64_t high = (std::int64_t{1} << width) - 1;
	if (value > high) {
		saturated = true;
		return static_cast<std::uint32_t>(high);
	}
	if (value < 0) {
		saturated = true;
		return 0;
	}
	return static_cast<std::uint32_t>(value);
}

std::int32_t sign_extend(std::uint32_t value, unsigned width)
{
	const auto shift = 32U - width;
	return static_cast<std::int32_t>(value << shift) >> shift;
}

std::uint32_t rotate_right(std::uint32_t value, unsigned amount)
{
	return std::rotr(value, static_cast<int>(amount & 31U));
}

/* The parallel operations, operation = prefix * 6 + suffix: prefix S, Q,
 * SH, U, UQ, UH; suffix ADD16, ASX, SAX, SUB16, ADD8, SUB8. */
void parallel(CpuState &cpu, std::uint64_t word, unsigned op)
{
	const unsigned prefix = op / 6U;
	const unsigned suffix = op % 6U;
	const bool unsigned_lanes = prefix >= 3U;
	const unsigned kind = prefix % 3U; /* 0 modulo, 1 saturating, 2 halving */
	const bool bytes = suffix >= 4U;
	const unsigned width = bytes ? 8U : 16U;
	const unsigned lanes = bytes ? 4U : 2U;
	const std::uint32_t n = read(cpu, word, tail::n_shift);
	const std::uint32_t m = read(cpu, word, tail::m_shift);
	const std::uint32_t mask = bytes ? 0xffU : 0xffffU;

	std::uint32_t result = 0;
	std::uint32_t ge = 0;
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const unsigned shift = lane * width;
		/* The lane of m the operation pairs with, and whether it adds. */
		unsigned m_lane = lane;
		bool add;
		switch (suffix) {
		case 0:
		case 4:
			add = true;
			break;
		case 3:
		case 5:
			add = false;
			break;
		case 1: /* ASX: the low half subtracts the high half of m */
			m_lane = 1U - lane;
			add = lane == 1;
			break;
		default: /* SAX */
			m_lane = 1U - lane;
			add = lane == 0;
			break;
		}
		const std::uint32_t a_raw = n >> shift & mask;
		const std::uint32_t b_raw = m >> (m_lane * width) & mask;
		const std::int64_t a = unsigned_lanes ?
			std::int64_t{a_raw} : std::int64_t{sign_extend(a_raw, width)};
		const std::int64_t b = unsigned_lanes ?
			std::int64_t{b_raw} : std::int64_t{sign_extend(b_raw, width)};
		const std::int64_t value = add ? a + b : a - b;
		std::uint32_t lane_result;
		bool saturated = false;
		switch (kind) {
		case 0:
			lane_result = static_cast<std::uint32_t>(value) & mask;
			break;
		case 1:
			lane_result = (unsigned_lanes ?
				unsigned_saturate(value, width, saturated) :
				static_cast<std::uint32_t>(
					signed_saturate(value, width, saturated))) &
				mask;
			break;
		default:
			lane_result = static_cast<std::uint32_t>(value >> 1) & mask;
			break;
		}
		result |= lane_result << shift;
		if (kind == 0) {
			const bool flag = unsigned_lanes && add ?
				value >= (std::int64_t{1} << width) : value >= 0;
			if (flag)
				ge |= mask << shift;
		}
	}
	if (kind == 0)
		cpu.a32_ge_mask = ge;
	write(cpu, word, tail::d_shift, result);
}

} // namespace

extern "C" void uml_arm64_a32_tail(CpuState *cpu_pointer,
				   std::uint64_t word) noexcept
{
	CpuState &cpu = *cpu_pointer;
	const unsigned op = static_cast<unsigned>(word >> tail::op_shift & 0xffU);
	if (op < 36U) {
		parallel(cpu, word, op);
		return;
	}
	const std::uint32_t n = read(cpu, word, tail::n_shift);
	const std::uint32_t m = read(cpu, word, tail::m_shift);
	const std::uint32_t a = read(cpu, word, tail::a_shift);
	const unsigned immediate = static_cast<unsigned>(word >> tail::imm_shift &
							 0xffU);
	bool q = false;
	switch (op) {
	case 36: /* SEL */
		write(cpu, word, tail::d_shift,
		      (n & cpu.a32_ge_mask) | (m & ~cpu.a32_ge_mask));
		return;
	case 37:
	case 38: { /* USAD8, USADA8 */
		std::uint32_t sum = op == 38 ? a : 0;
		for (unsigned lane = 0; lane < 4; ++lane) {
			const auto x = static_cast<std::int32_t>(n >> (8 * lane) & 0xffU);
			const auto y = static_cast<std::int32_t>(m >> (8 * lane) & 0xffU);
			sum += static_cast<std::uint32_t>(x > y ? x - y : y - x);
		}
		write(cpu, word, tail::d_shift, sum);
		return;
	}
	case 39:
	case 40: { /* SSAT16, USAT16 */
		std::uint32_t result = 0;
		for (unsigned lane = 0; lane < 2; ++lane) {
			const auto value = sign_extend(n >> (16 * lane) & 0xffffU, 16);
			const std::uint32_t lane_result = op == 39 ?
				static_cast<std::uint32_t>(
					signed_saturate(value, immediate, q)) :
				unsigned_saturate(value, immediate, q);
			result |= (lane_result & 0xffffU) << (16 * lane);
		}
		if (q)
			cpu.a32_q = 1;
		write(cpu, word, tail::d_shift, result);
		return;
	}
	case 41 ... 48: { /* SMUAD, SMUADX, SMUSD, SMUSDX, SMLAD, ... */
		const bool exchange = (op - 41U) % 2U != 0U;
		const bool subtract = op == 43 || op == 44 || op == 47 || op == 48;
		const bool accumulate = op >= 45;
		const std::uint32_t second = exchange ? rotate_right(m, 16) : m;
		const std::int64_t p1 = std::int64_t{sign_extend(n & 0xffffU, 16)} *
					sign_extend(second & 0xffffU, 16);
		const std::int64_t p2 = std::int64_t{sign_extend(n >> 16, 16)} *
					sign_extend(second >> 16, 16);
		std::int64_t result = subtract ? p1 - p2 : p1 + p2;
		if (accumulate)
			result += static_cast<std::int32_t>(a);
		if (result != static_cast<std::int32_t>(result))
			cpu.a32_q = 1;
		write(cpu, word, tail::d_shift, static_cast<std::uint32_t>(result));
		return;
	}
	case 49 ... 52: { /* SMLALD, SMLALDX, SMLSLD, SMLSLDX */
		const bool exchange = (op - 49U) % 2U != 0U;
		const bool subtract = op >= 51;
		const std::uint32_t second = exchange ? rotate_right(m, 16) : m;
		const std::int64_t p1 = std::int64_t{sign_extend(n & 0xffffU, 16)} *
					sign_extend(second & 0xffffU, 16);
		const std::int64_t p2 = std::int64_t{sign_extend(n >> 16, 16)} *
					sign_extend(second >> 16, 16);
		const std::uint64_t accumulator =
			static_cast<std::uint64_t>(read(cpu, word, tail::hi_shift)) << 32 |
			read(cpu, word, tail::lo_shift);
		const std::uint64_t result = accumulator +
			static_cast<std::uint64_t>(subtract ? p1 - p2 : p1 + p2);
		write(cpu, word, tail::lo_shift, static_cast<std::uint32_t>(result));
		write(cpu, word, tail::hi_shift,
		      static_cast<std::uint32_t>(result >> 32));
		return;
	}
	case 53 ... 56: { /* SXTB16, UXTB16, SXTAB16, UXTAB16 */
		const bool is_signed = op == 53 || op == 55;
		const bool add = op >= 55;
		const std::uint32_t rotated = rotate_right(m, immediate);
		std::uint32_t result = 0;
		for (unsigned lane = 0; lane < 2; ++lane) {
			const std::uint32_t byte = rotated >> (16 * lane) & 0xffU;
			std::uint32_t half = is_signed ?
				static_cast<std::uint32_t>(sign_extend(byte, 8)) :
				byte;
			if (add)
				half += n >> (16 * lane);
			result |= (half & 0xffffU) << (16 * lane);
		}
		write(cpu, word, tail::d_shift, result);
		return;
	}
	default: /* QADD, QSUB, QDADD, QDSUB */
		break;
	}
	/* QADD Rd, Rm, Rn and its relatives: m is the first operand. */
	std::int64_t second = static_cast<std::int32_t>(n);
	if (op >= 59) {
		second *= 2;
		second = signed_saturate(second, 32, q);
	}
	const std::int64_t first = static_cast<std::int32_t>(m);
	const std::int64_t value = (op == 58 || op == 60) ? first - second :
							      first + second;
	const auto result = signed_saturate(value, 32, q);
	if (q)
		cpu.a32_q = 1;
	write(cpu, word, tail::d_shift, static_cast<std::uint32_t>(result));
}
