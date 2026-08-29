// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/engine.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "memory/static_guest_memory.hpp"

#include <cstddef>
#include <memory>

namespace uml::runtime {

struct GuestInterruptSequence final {
	const unsigned int *sequence{};
	unsigned int snapshot{};
};

class ThreadEngineSlots final {
public:
	template<::uml::cpu::EngineKind Kind>
	[[nodiscard]] std::unique_ptr<::uml::cpu::CpuEngine> &slot() noexcept
	{
		if constexpr (Kind == ::uml::cpu::EngineKind::interpreter)
			return interpreter_;
		else if constexpr (Kind == ::uml::cpu::EngineKind::gadgets)
			return gadgets_;
		else if constexpr (Kind == ::uml::cpu::EngineKind::jit)
			return jit_;
		else {
			static_assert(Kind == ::uml::cpu::EngineKind::interpreter ||
				Kind == ::uml::cpu::EngineKind::gadgets ||
				Kind == ::uml::cpu::EngineKind::jit,
				"unsupported CPU engine kind");
			return interpreter_;
		}
	}

	template<::uml::cpu::EngineKind Kind>
	[[nodiscard]] const std::unique_ptr<::uml::cpu::CpuEngine> &slot()
		const noexcept
	{
		if constexpr (Kind == ::uml::cpu::EngineKind::interpreter)
			return interpreter_;
		else if constexpr (Kind == ::uml::cpu::EngineKind::gadgets)
			return gadgets_;
		else if constexpr (Kind == ::uml::cpu::EngineKind::jit)
			return jit_;
		else {
			static_assert(Kind == ::uml::cpu::EngineKind::interpreter ||
				Kind == ::uml::cpu::EngineKind::gadgets ||
				Kind == ::uml::cpu::EngineKind::jit,
				"unsupported CPU engine kind");
			return interpreter_;
		}
	}

	[[nodiscard]] bool empty() const noexcept
	{
		return interpreter_ == nullptr && gadgets_ == nullptr && jit_ == nullptr;
	}

	[[nodiscard]] std::size_t active_count() const noexcept
	{
		return static_cast<std::size_t>(interpreter_ != nullptr) +
			static_cast<std::size_t>(gadgets_ != nullptr) +
			static_cast<std::size_t>(jit_ != nullptr);
	}

private:
	std::unique_ptr<::uml::cpu::CpuEngine> interpreter_;
	std::unique_ptr<::uml::cpu::CpuEngine> gadgets_;
	std::unique_ptr<::uml::cpu::CpuEngine> jit_;
};

struct ThreadExecutionState final {
	::uml::cpu::arm64::CpuState cpu{};
	::uml::cpu::x86_64::CpuState x86_cpu{};
	::uml::memory::StaticGuestMemory memory;
	ThreadEngineSlots engines;
	GuestInterruptSequence interrupt;
};

} // namespace uml::runtime
