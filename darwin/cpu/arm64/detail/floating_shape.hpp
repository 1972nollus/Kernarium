// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace uml::cpu::arm64::detail {

/* The element width of an FP type field (ftype, bits 23:22). Type 2 is
 * reserved and each caller rejects it, so its width is never used. */
[[nodiscard]] constexpr unsigned floating_width_from_type(unsigned type)
	noexcept
{
	return type == 3U ? 16U : type == 0U ? 32U : 64U;
}

} // namespace uml::cpu::arm64::detail
