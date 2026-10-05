// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* x86's SIMD floating-point results for the JIT, where the host's arithmetic
 * differs: the cold path of FEXCore's EmbedderFPCheck op hands an operation's
 * x86 sources and the host's result here. The model computes in integers and
 * SoftFloat only, because it runs inside a dispatch, under the guest's FPCR.
 */

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::x86_64::jit {

/* The x86 operation of one instruction, or of one 128-bit part of it, as
 * FEXCore's EmbedderFPKind names it. */
enum class FpKind : std::uint8_t {
	add,
	sub,
	mul,
	div,
	min,
	max,
	sqrt,
	rcp,
	rsqrt,
};

struct FpOperation final {
	FpKind kind{};
	/* Of each lane, or of each source lane for a conversion: 2, 4 or 8. */
	std::uint32_t element_bytes{};
	/* The lanes the operation computes, from lane 0. */
	std::uint32_t lanes{};
	std::uint8_t immediate{};
	std::uint8_t flags{};
};

using FpVector = std::array<std::byte, 16>;

struct FpValues final {
	/* The sources in x86's order. */
	std::array<FpVector, 3> sources{};
	/* The host's result on entry, x86's on return: lanes the operation does
	 * not compute keep their value. */
	FpVector result{};
};

/* Replaces the computed lanes of `values.result` with x86's results under
 * `mxcsr`, and returns the MXCSR exception flags x86 raises for them. */
std::uint32_t x86_fp_compute(const FpOperation &operation,
	std::uint32_t mxcsr, FpValues &values) noexcept;

} // namespace uml::cpu::x86_64::jit
