// SPDX-License-Identifier: GPL-2.0-or-later
//
// The semantic library's BFloat16 extension: the conversions VCVT, VCVTB and
// VCVTT from single precision, the dot product VDOT, the matrix multiply
// accumulate VMMLA, and the widening multiply-adds VFMA. The arithmetic of
// the dot product and the matrix multiply is the architecture's own: each
// product and sum is computed exactly, rounded to odd to a 24-bit
// significand, with denormals flushed and no exceptions, which no host
// instruction is required to match, so it is the exact integer arithmetic
// the A64 forms share (cpu/arm64/detail/bfloat_dot.hpp), and so are the
// conversions (bfloat_conversion.hpp). The multiply-adds are the host's
// fused single-precision instruction on the exactly widened operands.

#include "cpu/arm64/aarch32/semantics_fp.hpp"
#include "cpu/arm64/aarch32/semantics_vector.hpp"
#include "cpu/arm64/detail/bfloat_conversion.hpp"
#include "cpu/arm64/detail/bfloat_dot.hpp"

#include <array>
#include <iterator>

namespace uml::cpu::arm64::aarch32::detail {

namespace {

using std::uint16_t;
using std::uint32_t;
using std::uint64_t;

using ::uml::cpu::arm64::detail::bfloat_dot_add;
using ::uml::cpu::arm64::detail::convert_to_bfloat;

/* VDOT, vector and by-element. */
bool vdot(Exec &x, bool by_element)
{
	const Operands &o = x.o;
	const unsigned index = by_element ? static_cast<unsigned>(o.i) : 0U;
	const uint64_t element_source = by_element ? x.d_reg(static_cast<unsigned>(o.m)) : uint64_t{0};
	return for_each_register(x, [&](Raw n, Raw m, Raw d) -> Raw {
		const Raw second = by_element ? element_source : m;
		Raw out = 0;
		for (unsigned e = 0; e < 2U; ++e) {
			const unsigned s = by_element ? index : e;
			out = with_lane(out, e, 32,
				bfloat_dot_add(static_cast<uint32_t>(lane(d, e, 32)),
					static_cast<uint16_t>(lane(n, 2U * e, 16)),
					static_cast<uint16_t>(lane(n, 2U * e + 1U, 16)),
					static_cast<uint16_t>(lane(second, 2U * s, 16)),
					static_cast<uint16_t>(lane(second, 2U * s + 1U, 16))));
		}
		return out;
	});
}

bool vdot_vector(Exec &x)
{
	return vdot(x, false);
}
bool vdot_element(Exec &x)
{
	return vdot(x, true);
}

/* VMMLA: a 2x4 by 4x2 matrix product of bfloat16 values accumulated into a
 * 2x2 matrix of singles. */
bool vmmla(Exec &x)
{
	const Operands &o = x.o;
	const u128 operand1 = q_reg(x, static_cast<unsigned>(o.n));
	const u128 operand2 = q_reg(x, static_cast<unsigned>(o.m));
	u128 result = q_reg(x, static_cast<unsigned>(o.d));
	const u128 addend = result;
	for (unsigned i = 0; i < 2U; ++i) {
		for (unsigned j = 0; j < 2U; ++j) {
			auto sum = static_cast<uint32_t>(lane128(addend, 2U * i + j, 32));
			for (unsigned k = 0; k < 2U; ++k)
				sum = bfloat_dot_add(sum, static_cast<uint16_t>(lane128(operand1, 4U * i + 2U * k, 16)),
					static_cast<uint16_t>(lane128(operand1, 4U * i + 2U * k + 1U, 16)),
					static_cast<uint16_t>(lane128(operand2, 4U * j + 2U * k, 16)),
					static_cast<uint16_t>(lane128(operand2, 4U * j + 2U * k + 1U, 16)));
			result = with_lane128(result, 2U * i + j, 32, sum);
		}
	}
	set_q_reg(x, static_cast<unsigned>(o.d), result);
	return true;
}

/* VFMA (BFloat16), vector and by-element: each single of the accumulator
 * gains the product of the bottom or top bfloat16 of the matching pair of
 * each source. */
bool vfma_bf16(Exec &x, bool by_element)
{
	const Operands &o = x.o;
	const unsigned select = static_cast<unsigned>(o.sel);
	const u128 operand1 = q_reg(x, static_cast<unsigned>(o.n));
	const u128 operand2 = by_element ? u128{0} : q_reg(x, static_cast<unsigned>(o.m));
	const uint64_t scalar_source = by_element ? x.d_reg(static_cast<unsigned>(o.m)) : uint64_t{0};
	u128 result = q_reg(x, static_cast<unsigned>(o.d));
	const u128 addend = result;
	fp::run_standard(x.cpu, [&] {
		for (unsigned e = 0; e < 4U; ++e) {
			const auto element1 = static_cast<uint32_t>(lane128(operand1, 2U * e + select, 16)) << 16;
			const auto element2 = by_element
				? static_cast<uint32_t>(lane(scalar_source, static_cast<unsigned>(o.i), 16)) << 16
				: static_cast<uint32_t>(lane128(operand2, 2U * e + select, 16)) << 16;
			result = with_lane128(result, e, 32,
				fp::fused(static_cast<uint32_t>(lane128(addend, e, 32)), element1, element2));
		}
		return 0;
	});
	set_q_reg(x, static_cast<unsigned>(o.d), result);
	return true;
}

bool vfma_bf(Exec &x)
{
	return vfma_bf16(x, false);
}
bool vfma_bfs(Exec &x)
{
	return vfma_bf16(x, true);
}

/* FPConvertBF(): a single to bfloat16 under the flush-to-zero, default-NaN
 * and rounding controls of `fpcr`; the flags it raises go to the FPSR. */
[[nodiscard]] uint16_t convert(CpuState &cpu, uint32_t fpcr, uint32_t operand) noexcept
{
	const auto converted = convert_to_bfloat(operand, fpcr);
	cpu.fpsr |= converted.exceptions;
	return converted.bits;
}

/* VCVT: four singles of Qm to four bfloat16 in Dd, under the Standard FPSCR
 * value. */
bool vcvt_bf16(Exec &x)
{
	const u128 source = q_reg(x, static_cast<unsigned>(x.o.m));
	const uint32_t fpcr = fp::standard_fpcr(x.cpu.fpcr);
	uint64_t result = 0;
	for (unsigned e = 0; e < 4U; ++e)
		result = with_lane(result, e, 16, convert(x.cpu, fpcr, static_cast<uint32_t>(lane128(source, e, 32))));
	x.set_d_reg(static_cast<unsigned>(x.o.d), result);
	return true;
}

/* VCVTB and VCVTT: one single to the bottom or top half of Sd, under the
 * FPSCR. */
template<unsigned Shift>
bool vcvt_bf16_half(Exec &x)
{
	const unsigned d = static_cast<unsigned>(x.o.d);
	const uint16_t half = convert(x.cpu, x.cpu.fpcr & fpscr_control_mask, x.s_reg(static_cast<unsigned>(x.o.m)));
	x.set_s_reg(d, (x.s_reg(d) & ~(0xffffU << Shift)) | uint32_t{half} << Shift);
	return true;
}

constexpr VectorEntry entries[] = {
	{"VDOT", vdot_vector}, {"VDOT_s", vdot_element}, {"VMMLA", vmmla},
	{"VFMA_bf", vfma_bf}, {"VFMA_bfs", vfma_bfs}, {"VCVT_bfs", vcvt_bf16},
	{"VCVTB_bfs", vcvt_bf16_half<0>}, {"VCVTT_bfs", vcvt_bf16_half<16>},
};

} // namespace

const VectorEntry *bf16_entries(std::size_t &count) noexcept
{
	count = std::size(entries);
	return entries;
}

} // namespace uml::cpu::arm64::aarch32::detail
