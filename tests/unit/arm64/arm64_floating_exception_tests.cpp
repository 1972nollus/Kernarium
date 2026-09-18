// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/detail/floating_exception.hpp"

#include <cstdint>
#include <cstdio>

/*
 * Both engines decide guest floating-point traps through
 * detail/floating_exception.hpp. This test pins that shared contract; the
 * engines' own trap behaviour is covered by arm64-floating-trap-control and
 * arm64-deferred-fp-state.
 */

namespace detail = uml::cpu::arm64::detail;

namespace {

constexpr std::uint32_t invalid = 1U << 0U;
constexpr std::uint32_t divide_by_zero = 1U << 1U;
constexpr std::uint32_t overflow = 1U << 2U;
constexpr std::uint32_t inexact = 1U << 4U;
constexpr std::uint32_t input_denormal = 1U << 7U;
constexpr std::uint32_t saturation = 1U << 27U;

constexpr std::uint32_t enable(std::uint32_t flags)
{
	return flags << 8U;
}

constexpr std::uint32_t fdiv_s0_s1_s2 = 0x1e22'1820U;
constexpr std::uint32_t ldr_q0_x0 = 0x3d40'0000U;
constexpr std::uint32_t add_x10_x10_1 = 0x9100'054aU;
/* size==00 is reserved in this Advanced-SIMD data-processing class. */
constexpr std::uint32_t reserved_simd = 0x2e00'fc00U;

/* The host FPCR never carries a guest trap enable. */
static_assert(detail::host_fpcr(0xffff'ffffU) ==
	(0xffff'ffffU & ~detail::fpcr_exception_enable_mask));
static_assert(detail::enabled_exception_flags(0x0000'9f00U) == 0x9fU);
static_assert(detail::enabled_exception_flags(enable(overflow)) == overflow);

/*
 * Only the register-only FP/SIMD data-processing space is transactional.
 * A reserved encoding in that space stays in it: the engines reject it
 * before execution, so enables never turn it into a trap.
 */
static_assert(detail::is_fp_simd_data_processing(fdiv_s0_s1_s2));
static_assert(detail::is_fp_simd_data_processing(reserved_simd));
static_assert(!detail::is_fp_simd_data_processing(ldr_q0_x0));
static_assert(!detail::is_fp_simd_data_processing(add_x10_x10_1));
static_assert(detail::needs_floating_exception_transaction(
	enable(divide_by_zero), fdiv_s0_s1_s2));
static_assert(!detail::needs_floating_exception_transaction(
	0U, fdiv_s0_s1_s2));
static_assert(!detail::needs_floating_exception_transaction(
	detail::fpcr_exception_enable_mask, ldr_q0_x0));
static_assert(!detail::needs_floating_exception_transaction(
	detail::fpcr_exception_enable_mask, add_x10_x10_1));

/* ESR_ELx: EC 0x2c, IL, TFV, then the raised enabled cause flags. */
static_assert(detail::floating_exception_syndrome(divide_by_zero) ==
	((0x2cU << 26U) | (1U << 25U) | (1U << 23U) | divide_by_zero));
static_assert(detail::floating_exception_syndrome(saturation | overflow) ==
	detail::floating_exception_syndrome(overflow));

/* FDIV 1/0 with DZE: the enabled cause traps and FPSR stays unchanged. */
constexpr auto divide_trap = detail::decide_floating_exception(
	enable(divide_by_zero), saturation | input_denormal | invalid,
	divide_by_zero);
static_assert(divide_trap.traps());
static_assert(divide_trap.newly_raised == divide_by_zero);
static_assert(divide_trap.raised_enabled == divide_by_zero);
static_assert(divide_trap.committed_fpsr ==
	(saturation | input_denormal | invalid));
static_assert(divide_trap.syndrome ==
	detail::floating_exception_syndrome(divide_by_zero));

/*
 * FMUL overflow with only OFE: the overflow traps, and the trap commits
 * only the newly raised unenabled inexact flag.
 */
constexpr auto overflow_trap = detail::decide_floating_exception(
	enable(overflow), input_denormal, overflow | inexact);
static_assert(overflow_trap.traps());
static_assert(overflow_trap.raised_enabled == overflow);
static_assert(overflow_trap.committed_fpsr == (input_denormal | inexact));
static_assert(overflow_trap.syndrome ==
	detail::floating_exception_syndrome(overflow));

/* FDIV 1/0 with only IOE: an unenabled cause commits normally. */
constexpr auto divide_commit = detail::decide_floating_exception(
	enable(invalid), input_denormal, divide_by_zero);
static_assert(!divide_commit.traps());
static_assert(divide_commit.raised_enabled == 0U);
static_assert(divide_commit.committed_fpsr ==
	(input_denormal | divide_by_zero));

/*
 * The isolated FPSR starts with every cumulative flag clear, so a flag that
 * was already set does not trap again unless the instruction raises it.
 */
constexpr auto already_set = detail::decide_floating_exception(
	enable(divide_by_zero), divide_by_zero, 0U);
static_assert(!already_set.traps());
static_assert(already_set.committed_fpsr == divide_by_zero);

/* Only the cumulative exception bits of the isolated FPSR can trap. */
constexpr auto saturation_only = detail::decide_floating_exception(
	detail::fpcr_exception_enable_mask, 0U, saturation);
static_assert(!saturation_only.traps());
static_assert(saturation_only.newly_raised == 0U);

} // namespace

int main()
{
	std::puts("arm64-floating-exception: shared trap contract holds");
	return 0;
}
