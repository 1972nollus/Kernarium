// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "program.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace uml::cpu::x86_64::gadgets {

/* Append the block's stream to `stream`, which the caller may hand back with
 * its capacity intact so a translation does not buy the storage again.
 * Returns the index of the first gadget the bank has no form for, leaving a
 * partial stream, or nothing when the whole block was appended. */
[[nodiscard]] std::optional<std::size_t> append_native_stream(
	std::vector<std::uintptr_t> &stream, std::span<const Gadget> gadgets,
	std::uint64_t final_rip, ExecutionMode mode,
	ProgramLinkResolver links = {});

[[nodiscard]] std::vector<std::uintptr_t> build_native_stream(
	std::span<const Gadget> gadgets, std::uint64_t final_rip,
	ExecutionMode mode, ProgramLinkResolver links = {});

} // namespace uml::cpu::x86_64::gadgets
