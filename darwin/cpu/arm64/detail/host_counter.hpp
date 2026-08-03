// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::cpu::arm64::detail {

/*
 * Apple Arm64 exposes the architectural virtual counter to EL0.  Reading the
 * matching frequency and count registers keeps the guest pair coherent while
 * avoiding a host-library clock conversion in the interpreter.
 */
[[nodiscard]] inline std::uint64_t read_host_counter_frequency() noexcept
{
	std::uint64_t value{};
	asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
	return value;
}

[[nodiscard]] inline std::uint64_t read_host_virtual_counter() noexcept
{
	std::uint64_t value{};
	asm volatile("mrs %0, cntvct_el0" : "=r"(value));
	return value;
}

} // namespace uml::cpu::arm64::detail
