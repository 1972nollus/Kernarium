// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/engine.hpp"
#include "memory/static_guest_memory.hpp"

#include <array>
#include <memory>

namespace uml::runtime {

struct GuestInterruptSequence final {
	const unsigned int *sequence{};
	unsigned int snapshot{};
};

struct ThreadExecutionState final {
	::uml::cpu::arm64::CpuState cpu{};
	::uml::memory::StaticGuestMemory memory;
	std::array<std::unique_ptr<::uml::cpu::CpuEngine>, 3> engines;
	GuestInterruptSequence interrupt;
};

} // namespace uml::runtime
