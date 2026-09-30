// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlb.hpp"

#include "../cpu_state.hpp"
#include "../detail/memory_copy_set.hpp"

#include "uml_darwin_static_mmu.h"
#include "uml_darwin_guest.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

namespace uml::memory {
namespace {

using ::uml::cpu::arm64::CpuState;

[[nodiscard]] std::uint64_t &cpu_slot(
	CpuState *cpu, std::uint16_t offset) noexcept
{
	return *reinterpret_cast<std::uint64_t *>(
		reinterpret_cast<std::byte *>(cpu) + offset);
}

enum class Ls64Operation {
	load,
	store,
	store_status,
	store_status_accdata,
};

std::atomic_flag ls64_lock = ATOMIC_FLAG_INIT;

class Ls64Lock final {
public:
	Ls64Lock() noexcept
	{
		while (ls64_lock.test_and_set(std::memory_order_acquire)) {
			while (ls64_lock.test(std::memory_order_relaxed))
				__asm__ volatile("yield");
		}
	}
	~Ls64Lock() { ls64_lock.clear(std::memory_order_release); }
	Ls64Lock(const Ls64Lock &) = delete;
	Ls64Lock &operator=(const Ls64Lock &) = delete;
};

template <Ls64Operation operation, bool base_sp, bool status_discard = false>
[[nodiscard]] std::uint32_t ls64_transfer(CpuState *cpu, Tlb *tlb,
	std::uintptr_t operand) noexcept
{
	/* The gadget engine owns both pointers and guarantees that they are valid,
	 * so production LS64 transfers carry no always-not-taken validation
	 * branches. Translation also packs direct CpuState byte offsets, so the
	 * hot helper does not decode architectural register numbers.
	 */
	const auto first_offset = static_cast<std::uint16_t>(operand);
	const auto base_offset = static_cast<std::uint16_t>(operand >> 16);
	const auto status_offset = static_cast<std::uint16_t>(operand >> 32);
	auto *const transfer = reinterpret_cast<std::byte *>(cpu) + first_offset;
	const auto tagged_address =
		base_sp ? cpu->sp : cpu_slot(cpu, base_offset);
	const auto address = cpu->untag_data_address(tagged_address, base_sp);
	constexpr bool load = operation == Ls64Operation::load;
	if ((address & 63U) != 0) {
		tlb->fault_alignment = 1;
		tlb->fault_address = address;
		tlb->fault_access = load ? Access::read : Access::write;
		return 0;
	}

	Ls64Lock lock;
	auto *pointer = tlb->translate(
		address, load ? Access::read : Access::write);
	if (pointer == nullptr)
		return 0;
	if constexpr (load) {
		std::memcpy(transfer, pointer, 64);
	} else {
		if constexpr (operation ==
			      Ls64Operation::store_status_accdata) {
			std::array<std::byte, 64> bytes;
			std::memcpy(bytes.data(), transfer, bytes.size());
			std::memcpy(
				bytes.data(), &cpu->accdata, sizeof(cpu->accdata));
			std::memcpy(pointer, bytes.data(), bytes.size());
		} else {
			std::memcpy(pointer, transfer, 64);
		}
		if constexpr (operation == Ls64Operation::store_status ||
			      operation == Ls64Operation::store_status_accdata) {
			if constexpr (!status_discard)
				cpu_slot(cpu, status_offset) = 0;
		}
	}
	return 1;
}

using ::uml::cpu::arm64::detail::MopsFamily;
using ::uml::cpu::arm64::detail::MopsOutcome;

template <MopsFamily family>
void mops_prologue(CpuState *cpu, std::uintptr_t operand) noexcept
{
	const auto destination_offset = static_cast<std::uint16_t>(operand);
	const auto count_offset = static_cast<std::uint16_t>(operand >> 16);
	const auto source_offset = static_cast<std::uint16_t>(operand >> 32);
	::uml::cpu::arm64::detail::mops_prologue<family>(*cpu,
		cpu_slot(cpu, destination_offset), cpu_slot(cpu, count_offset),
		cpu_slot(cpu, source_offset));
}

/* Returns 1 on completion, 0 for a fault with the registers committed at the
 * last chunk boundary, and 2 for an epilogue the prologue cannot have
 * formatted. */
template <MopsFamily family, bool source_zero = false>
[[nodiscard]] std::uint32_t mops_epilogue(CpuState *cpu, Tlb *tlb,
	std::uintptr_t operand) noexcept
{
	const auto destination_offset = static_cast<std::uint16_t>(operand);
	const auto count_offset = static_cast<std::uint16_t>(operand >> 16);
	const auto source_offset = static_cast<std::uint16_t>(operand >> 32);
	std::uint64_t zero{};
	auto &source = source_zero ? zero : cpu_slot(cpu, source_offset);
	switch (::uml::cpu::arm64::detail::mops_epilogue<family>(*cpu, *tlb,
		cpu_slot(cpu, destination_offset), cpu_slot(cpu, count_offset),
		source)) {
	case MopsOutcome::complete:
		return 1;
	case MopsOutcome::fault:
		return 0;
	case MopsOutcome::illegal:
		break;
	}
	return 2;
}

} // namespace

} // namespace uml::memory

#define UML_LS64_WRAPPER(name, operation, base_sp, status_discard) \
extern "C" std::uint32_t uml_arm64_gadget_ls64_transfer_##name( \
	uml::cpu::arm64::CpuState *cpu, \
	::uml::memory::Tlb *tlb, \
	std::uintptr_t operand) noexcept \
{ \
	using namespace uml::memory; \
	return ls64_transfer<Ls64Operation::operation, base_sp, status_discard>( \
		cpu, tlb, operand); \
}

UML_LS64_WRAPPER(load_r, load, false, false)
UML_LS64_WRAPPER(load_s, load, true, false)
UML_LS64_WRAPPER(store_r, store, false, false)
UML_LS64_WRAPPER(store_s, store, true, false)
UML_LS64_WRAPPER(st64bv_rr, store_status, false, false)
UML_LS64_WRAPPER(st64bv_rz, store_status, false, true)
UML_LS64_WRAPPER(st64bv_sr, store_status, true, false)
UML_LS64_WRAPPER(st64bv_sz, store_status, true, true)
UML_LS64_WRAPPER(st64bv0_rr, store_status_accdata, false, false)
UML_LS64_WRAPPER(st64bv0_rz, store_status_accdata, false, true)
UML_LS64_WRAPPER(st64bv0_sr, store_status_accdata, true, false)
UML_LS64_WRAPPER(st64bv0_sz, store_status_accdata, true, true)

#undef UML_LS64_WRAPPER

#define UML_MOPS_PROLOGUE_WRAPPER(name, operation) \
extern "C" void uml_arm64_gadget_mops_prologue_transfer_##name( \
	uml::cpu::arm64::CpuState *cpu, std::uintptr_t operand) noexcept \
{ \
	using namespace uml::memory; \
	mops_prologue<MopsFamily::operation>(cpu, operand); \
}

#define UML_MOPS_EPILOGUE_WRAPPER(name, operation, source_zero) \
extern "C" std::uint32_t uml_arm64_gadget_mops_epilogue_transfer_##name( \
	uml::cpu::arm64::CpuState *cpu, \
	::uml::memory::Tlb *tlb, \
	std::uintptr_t operand) noexcept \
{ \
	using namespace uml::memory; \
	return mops_epilogue<MopsFamily::operation, source_zero>( \
		cpu, tlb, operand); \
}

UML_MOPS_PROLOGUE_WRAPPER(copy_forward, copy_forward)
UML_MOPS_PROLOGUE_WRAPPER(copy_general, copy_general)
UML_MOPS_PROLOGUE_WRAPPER(set, set)
UML_MOPS_EPILOGUE_WRAPPER(copy_forward, copy_forward, false)
UML_MOPS_EPILOGUE_WRAPPER(copy_general, copy_general, false)
UML_MOPS_EPILOGUE_WRAPPER(set_r, set, false)
UML_MOPS_EPILOGUE_WRAPPER(set_z, set, true)

#undef UML_MOPS_PROLOGUE_WRAPPER
#undef UML_MOPS_EPILOGUE_WRAPPER

