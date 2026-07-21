// SPDX-License-Identifier: GPL-2.0-or-later
#include "../dispatcher.hpp"
#include "program.hpp"
#include "tlb.hpp"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace uml::backend::arm64::gadgets {
namespace {

constexpr std::size_t page_size = 4096;

class TlbMemory final : public GuestMemory {
public:
	explicit TlbMemory(Tlb &tlb) noexcept : tlb_{tlb} {}

	bool read(std::uint64_t address,
		  std::span<std::byte> destination) override
	{
		return transfer(address, destination, Access::read);
	}

	bool write(std::uint64_t address,
		   std::span<const std::byte> source) override
	{
		/* Validate every page before the first store so a faulting ARM
		 * instruction never leaves a partially modified guest range.
		 */
		std::array<std::byte *, 2> destinations{};
		std::array<std::size_t, 2> lengths{};
		std::size_t count = 0;
		std::size_t completed = 0;
		while (completed < source.size()) {
			if (count == destinations.size())
				return false;
			const auto current = address + completed;
			auto *translated = tlb_.translate(current, Access::write);
			if (translated == nullptr)
				return false;
			const auto length = std::min(
				source.size() - completed,
				page_size - static_cast<std::size_t>(current % page_size));
			destinations[count] = translated;
			lengths[count] = length;
			++count;
			completed += length;
		}
		completed = 0;
		for (std::size_t index = 0; index < count; ++index) {
			std::memcpy(destinations[index], source.data() + completed,
				lengths[index]);
			completed += lengths[index];
		}
		return true;
	}

	std::byte *atomic_pointer(std::uint64_t address, std::size_t size,
				  MemoryAccess access) override
	{
		if (size == 0 || size > page_size -
				static_cast<std::size_t>(address % page_size))
			return nullptr;
		return tlb_.translate(address,
			access == MemoryAccess::write ? Access::write : Access::read);
	}

	MemoryFault last_fault() const noexcept override
	{
		return {tlb_.fault_address,
			tlb_.fault_access == Access::write ? MemoryAccess::write :
				MemoryAccess::read};
	}

	void invalidate_code(std::uint64_t address) override
	{
		darwin_uml_static_vm_invalidate_code(
			static_cast<unsigned long>(address));
	}

private:
	bool transfer(std::uint64_t address, std::span<std::byte> destination,
		      Access access)
	{
		while (!destination.empty()) {
			auto *translated = tlb_.translate(address, access);
			if (translated == nullptr)
				return false;
			const auto count = std::min(
				destination.size(), page_size - static_cast<std::size_t>(
					address % page_size));
			std::memcpy(destination.data(), translated, count);
			address += count;
			destination = destination.subspan(count);
		}
		return true;
	}

	Tlb &tlb_;
};

} // namespace
} // namespace uml::backend::arm64::gadgets

extern "C" int uml_arm64_gadget_execute_decoded(
	uml::backend::arm64::CpuState *cpu,
	uml::backend::arm64::gadgets::Tlb *tlb,
	std::uint32_t instruction) noexcept
{
	using uml::backend::arm64::Dispatcher;
	using uml::backend::arm64::StopReason;
	using uml::backend::arm64::gadgets::Event;
	using uml::backend::arm64::gadgets::TlbMemory;

	if (cpu == nullptr || tlb == nullptr)
		return static_cast<int>(Event::fatal);
	TlbMemory memory{*tlb};
	const auto result = Dispatcher{}.execute(*cpu, memory, instruction);
	switch (result.reason) {
	case StopReason::continued:
		return static_cast<int>(Event::continued);
	case StopReason::system_call:
		return static_cast<int>(Event::system_call);
	case StopReason::memory_fault:
		return static_cast<int>(Event::memory_fault);
	case StopReason::budget_exhausted:
		return static_cast<int>(Event::interrupt);
	case StopReason::illegal_instruction:
		return static_cast<int>(Event::illegal_instruction);
	case StopReason::breakpoint:
	case StopReason::unsupported_instruction:
		return static_cast<int>(Event::fatal);
	}
	return static_cast<int>(Event::fatal);
}
