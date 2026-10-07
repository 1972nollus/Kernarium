// SPDX-License-Identifier: GPL-2.0-or-later
/* The C++ half of the gadget engine's SVE part (engine/sve.inc): the
 * sve_library gadget runs an SVE word that no generated gadget lowers, or a
 * memory access off a generated gadget's fast path, through the shared
 * semantic library (cpu/arm64/detail/sve_semantics.hpp) on the register
 * file. The gadget has stored the guest's NZCV and, while the native FP
 * environment is active, its cumulative FPSR flags in CpuState first, so
 * the library sees no older state than the guest's; it reloads both after. */
#include "sve_layout.h"
#include "tlb.hpp"

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/detail/sme_encodings.hpp"
#include "cpu/arm64/detail/sve_semantics.hpp"

#include <cstddef>
#include <cstdint>

namespace {

using uml::cpu::arm64::CpuState;
namespace detail = uml::cpu::arm64::detail;

/* Guest memory through the executor's TLB. The library forms every
 * address untagged, so the plain translation applies. A miss there records
 * its fault in the TLB, which a first-fault or non-fault load's probe may
 * leave behind; the helper replaces it with the library's own report
 * whenever the instruction faults, so no probe's record reaches the exit. */
class GadgetMemory final : public detail::SveMemory {
public:
	explicit GadgetMemory(uml::memory::Tlb &tlb) noexcept : tlb_{tlb} {}

	std::byte *translate(std::uint64_t address,
		uml::memory::MemoryAccess access) noexcept override
	{
		return tlb_.translate(address,
			uml::memory::translated_access(access));
	}

	/* The library clears the local exclusive monitor itself; a gadget
	 * store gives no other notice either. */
	void stored(std::byte *, std::size_t) noexcept override {}

private:
	uml::memory::Tlb &tlb_;
};

} // namespace

/* Runs the SVE word in the low half of `operand`, whose table row is
 * detail::sme_encodings[operand >> 32], and returns an
 * UML_GADGET_SVE_LIBRARY_* outcome; a memory fault is in the TLB. */
extern "C" [[gnu::noinline]] std::uint32_t uml_arm64_gadget_sve_execute(
	CpuState *cpu, uml::memory::Tlb *tlb, std::uint64_t operand) noexcept
{
	const auto word = static_cast<std::uint32_t>(operand);
	const auto index = static_cast<std::size_t>(operand >> 32);
	if (cpu == nullptr || tlb == nullptr ||
	    index >= detail::sme_encodings.size())
		return UML_GADGET_SVE_LIBRARY_FATAL;
	GadgetMemory memory{*tlb};
	const auto result = detail::execute_sve(*cpu, word,
		detail::sme_encodings[index], memory);
	switch (result.event) {
	case detail::SveEvent::retired:
		return UML_GADGET_SVE_LIBRARY_RETIRED;
	case detail::SveEvent::memory_fault:
		tlb->fault_address = result.fault_address;
		tlb->fault_access =
			uml::memory::translated_access(result.access);
		tlb->fault_alignment = result.alignment ? 1U : 0U;
		return UML_GADGET_SVE_LIBRARY_FAULT;
	case detail::SveEvent::undefined:
		return UML_GADGET_SVE_LIBRARY_UNDEFINED;
	case detail::SveEvent::floating_point_trap:
		/* Trap enables run each instruction alone, through the library
		 * from the executor (run_trap_enabled_instruction); with them
		 * clear nothing traps. */
	case detail::SveEvent::fatal:
		break;
	}
	return UML_GADGET_SVE_LIBRARY_FATAL;
}
