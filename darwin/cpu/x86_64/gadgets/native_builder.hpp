// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "program.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace uml::cpu::x86_64::gadgets {

[[nodiscard]] std::vector<std::uintptr_t> build_native_stream(
	std::span<const Gadget> gadgets, std::uint64_t final_rip,
	ExecutionMode mode, ProgramLinkResolver links = {});

} // namespace uml::cpu::x86_64::gadgets
