// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/engine.hpp"
#if DARWIN_UML_X86_64_GUEST
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
#endif
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
		if constexpr (Kind == ::uml::cpu::EngineKind::gadgets)
			return gadgets_;
		else if constexpr (Kind == ::uml::cpu::EngineKind::jit)
			return jit_;
		else {
			static_assert(Kind == ::uml::cpu::EngineKind::gadgets ||
				Kind == ::uml::cpu::EngineKind::jit,
				"unsupported CPU engine kind");
			return gadgets_;
		}
	}

	template<::uml::cpu::EngineKind Kind>
	[[nodiscard]] const std::unique_ptr<::uml::cpu::CpuEngine> &slot()
		const noexcept
	{
		if constexpr (Kind == ::uml::cpu::EngineKind::gadgets)
			return gadgets_;
		else if constexpr (Kind == ::uml::cpu::EngineKind::jit)
			return jit_;
		else {
			static_assert(Kind == ::uml::cpu::EngineKind::gadgets ||
				Kind == ::uml::cpu::EngineKind::jit,
				"unsupported CPU engine kind");
			return gadgets_;
		}
	}

	[[nodiscard]] bool empty() const noexcept
	{
		return gadgets_ == nullptr && jit_ == nullptr;
	}

	[[nodiscard]] std::size_t active_count() const noexcept
	{
		return static_cast<std::size_t>(gadgets_ != nullptr) +
			static_cast<std::size_t>(jit_ != nullptr);
	}

private:
	std::unique_ptr<::uml::cpu::CpuEngine> gadgets_;
	std::unique_ptr<::uml::cpu::CpuEngine> jit_;
};

struct ThreadExecutionState final {
	::uml::cpu::arm64::CpuState cpu{};
#if DARWIN_UML_X86_64_GUEST
	::uml::cpu::x86_64::CpuState x86_cpu{};
	::uml::cpu::x86_64::gadgets::Executor x86_executor{};
	/* Made the first time the guest enables #MF or #XM. */
	std::unique_ptr<::uml::cpu::x86_64::gadgets::Executor>
		x86_checked_executor;
#endif
	::uml::memory::StaticGuestMemory memory;
	ThreadEngineSlots engines;
	GuestInterruptSequence interrupt;
};

} // namespace uml::runtime
