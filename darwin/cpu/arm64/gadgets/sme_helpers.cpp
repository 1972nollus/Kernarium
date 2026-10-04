// SPDX-License-Identifier: GPL-2.0-or-later
/* The C++ halves of the gadget engine's SME instructions (engine/sme.inc).
 * They run with the host out of streaming mode and work on the vCPU's
 * register file, where guest ZA and ZT0 are whenever host code runs. */
#include "sme_layout.h"
#include "tlb.hpp"

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/memory_transfer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

using uml::cpu::arm64::CpuState;
using uml::memory::Access;

/* Copy `size` bytes between guest memory at `address` and `data`. Both pages
 * a transfer can touch are translated before any byte moves, so a fault
 * leaves memory and the register file as they were and reports the first
 * byte of the faulting page. */
[[nodiscard]] bool transfer(uml::memory::Tlb &tlb, std::uint64_t address,
	std::byte *data, std::size_t size, Access access) noexcept
{
	constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
	std::array<std::byte *, 2> pages{};
	std::array<std::size_t, 2> lengths{};
	std::size_t count = 0;
	for (std::size_t done = 0; done < size; ++count) {
		if (count == pages.size())
			return false;
		const auto current = address + done;
		pages[count] = tlb.translate_tagged(current, access);
		if (pages[count] == nullptr)
			return false;
		lengths[count] = std::min(size - done,
			page_size - static_cast<std::size_t>(current % page_size));
		done += lengths[count];
	}
	for (std::size_t index = 0, done = 0; index < count; ++index) {
		if (access == Access::write)
			std::memcpy(pages[index], data + done, lengths[index]);
		else
			std::memcpy(data + done, pages[index], lengths[index]);
		done += lengths[index];
	}
	return true;
}

} // namespace

/* A guest write of SVCR, with the bits in the operand's low byte taking the
 * values in its next. Leaves the native FP environment first: the write can
 * set the guest FPSR, which the environment would otherwise overwrite when it
 * is next restored. A changed mode empties the return ring, whose records
 * name code translated for the old one. */
extern "C" [[gnu::noinline]] std::uint32_t uml_arm64_gadget_sme_write_svcr(
	CpuState *cpu, std::uint64_t operand) noexcept
{
	if (cpu->sme_registers == nullptr)
		return UML_GADGET_SME_SVCR_FATAL;
	uml::cpu::arm64::restore_native_fp_environment(*cpu);
	const auto previous = cpu->svcr;
	if (!cpu->write_svcr(static_cast<std::uint8_t>(operand),
		    static_cast<std::uint8_t>(
			    operand >> UML_GADGET_SME_SVCR_VALUE_SHIFT)))
		return UML_GADGET_SME_SVCR_NEEDS_STORAGE;
	if (cpu->svcr != previous) {
		cpu->gadget_return_stack = {};
		cpu->gadget_return_top = 0;
	}
	return UML_GADGET_SME_SVCR_WRITTEN;
}

/* Every ZA and ZT0 access and zeroing, on the register file, as the
 * descriptor (sme_layout.h) names it. Returns 0 after recording a fault in
 * the TLB, which then reports it as the native access would: an SP base
 * that is not 16-byte aligned, or the first byte of a page the access
 * cannot reach. */
extern "C" [[gnu::noinline]] std::uint32_t uml_arm64_gadget_sme_memory(
	CpuState *cpu, uml::memory::Tlb *tlb, std::uint64_t descriptor) noexcept
{
	const std::size_t svl = cpu->sme_svl;
	std::byte *const file = cpu->sme_registers;
	const auto operation = static_cast<unsigned>(
		(descriptor >> UML_GADGET_SME_OPERATION_SHIFT) & 7U);
	if (operation == UML_GADGET_SME_ZA_ZERO) {
		const auto mask = static_cast<unsigned>(
			(descriptor >> UML_GADGET_SME_MASK_SHIFT) & 0xffU);
		/* ZAk.D holds the array vectors whose number is k modulo 8. */
		for (std::size_t row = 0; row < svl; ++row)
			if ((mask >> (row % 8U)) & 1U)
				std::memset(file + uml::cpu::arm64::sme_za_offset(
					svl, static_cast<unsigned>(row)), 0, svl);
		return 1;
	}
	if (operation == UML_GADGET_SME_ZT0_ZERO) {
		std::memset(file + uml::cpu::arm64::sme_zt0_offset(svl), 0,
			uml::cpu::arm64::sme_zt0_bytes);
		return 1;
	}
	const bool base_sp =
		((descriptor >> UML_GADGET_SME_BASE_SP_BIT) & 1U) != 0U;
	const auto access =
		operation == UML_GADGET_SME_ZA_STORE ||
		operation == UML_GADGET_SME_ZT0_STORE ? Access::write : Access::read;
	std::uint64_t base;
	std::memcpy(&base, reinterpret_cast<const std::byte *>(cpu) +
		(descriptor & 0xffffU), sizeof(base));
	if (base_sp && (base & 0xfU) != 0U) {
		uml_arm64_alignment_fault(tlb, base,
			static_cast<std::uint32_t>(access));
		return 0;
	}
	std::uint64_t address = base;
	std::byte *data;
	std::size_t size;
	if (operation == UML_GADGET_SME_ZA_LOAD ||
	    operation == UML_GADGET_SME_ZA_STORE) {
		const auto offset = static_cast<std::uint32_t>(
			(descriptor >> 32) & 0xfU);
		std::uint32_t index;
		std::memcpy(&index, reinterpret_cast<const std::byte *>(cpu) +
			((descriptor >> 16) & 0xffffU), sizeof(index));
		address += std::uint64_t{offset} * svl;
		const auto row = static_cast<unsigned>(
			(index + offset) & (svl - 1U));
		data = file + uml::cpu::arm64::sme_za_offset(svl, row);
		size = svl;
	} else {
		data = file + uml::cpu::arm64::sme_zt0_offset(svl);
		size = uml::cpu::arm64::sme_zt0_bytes;
	}
	address = cpu->untag_data_address(address, base_sp);
	if (!transfer(*tlb, address, data, size, access))
		return 0;
	if (access == Access::write)
		cpu->exclusive_size = 0;
	return 1;
}
