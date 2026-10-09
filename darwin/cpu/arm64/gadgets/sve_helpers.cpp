// SPDX-License-Identifier: GPL-2.0-or-later
/* The C++ half of the gadget engine's SVE part (engine/sve.inc): the
 * sve_library gadget runs an SVE word that no generated gadget lowers, or a
 * memory access off a generated gadget's fast path, through the shared
 * semantic library (cpu/arm64/detail/sve_semantics.hpp) on the register
 * file. The gadget has stored the guest's NZCV and, while the native FP
 * environment is active, its cumulative FPSR flags in CpuState first, so
 * the library sees no older state than the guest's; it reloads both after.
 * The executor runs each SVE word through the library directly while an
 * FPCR trap enable is set (run_trap_enabled_instruction). */
#include "sve_helpers.hpp"
#include "sve_layout.h"

#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/sve_semantics_forms.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/arm64/sve_profile.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64::gadgets {
namespace {

/* Guest memory through the executor's TLB. The library forms every
 * address untagged, so the plain translation applies. A miss there records
 * its fault in the TLB, which a first-fault or non-fault load's probe may
 * leave behind; execute_sve_word replaces it with the library's own report
 * whenever the instruction faults, so no probe's record reaches the exit. */
class GadgetMemory final : public detail::SveMemory {
public:
	explicit GadgetMemory(memory::Tlb &tlb) noexcept : tlb_{tlb} {}

	std::byte *translate(std::uint64_t address,
		memory::MemoryAccess access) noexcept override
	{
		return tlb_.translate(address, memory::translated_access(access));
	}

	/* The library clears the local exclusive monitor itself. Guest code
	 * synchronizes its own instruction stream after writing it, so a
	 * store gives no other notice, as a gadget store gives none. */
	void stored(std::byte *, std::size_t) noexcept override {}

private:
	memory::Tlb &tlb_;
};

/* Whether `word` of `row` is UNDEFINED by its encoding alone: a bitmask
 * immediate form (AND, ORR, EOR and DUPM) with a reserved immediate. These
 * are the only words the semantic library reports UNDEFINED, so the
 * builder ends the block in front of them, before any of the program's
 * state, the live marker included, changes. */
[[nodiscard]] bool reserved_encoding(std::uint32_t word,
	const detail::SmeEncoding &row) noexcept
{
	namespace form = detail::sve_semantics::form;
	if (row.form != form::and_z_zi && row.form != form::orr_z_zi &&
	    row.form != form::eor_z_zi && row.form != form::dupm_z_i)
		return false;
	return !detail::decode_logical_immediate(true, (word >> 17) & 1U,
		(word >> 11) & 0x3fU, (word >> 5) & 0x3fU).has_value();
}

} // namespace

SveWordDecode decode_sve_word(std::uint32_t word,
	const HostCapabilities &host, std::uint8_t svcr) noexcept
{
	const auto *row = detail::find_sme_encoding(word);
	if (row == nullptr ||
	    (row->non_streaming_requirement & detail::sme_feature_sve) == 0U)
		return {};
	const auto features = detail::implemented_sve_features(host);
	if ((row->non_streaming_requirement & features) !=
		    row->non_streaming_requirement ||
	    ((row->mode & detail::sme_mode_za) != 0U &&
		    (svcr & svcr_za) == 0U) ||
	    reserved_encoding(word, *row))
		return {.kind = SveWord::undefined, .row = row};
	return {.kind = SveWord::implemented, .row = row};
}

detail::SveResult execute_sve_word(CpuState &cpu, memory::Tlb &tlb,
	std::uint32_t word, const detail::SmeEncoding &row) noexcept
{
	GadgetMemory memory{tlb};
	const auto result = detail::execute_sve(cpu, word, row, memory);
	if (result.event == detail::SveEvent::memory_fault) {
		tlb.fault_address = result.fault_address;
		tlb.fault_access = memory::translated_access(result.access);
		tlb.fault_alignment = result.alignment ? 1U : 0U;
	}
	return result;
}

} // namespace uml::cpu::arm64::gadgets

/* Runs the SVE word in the high half of `operand`, whose table row is
 * detail::sme_encodings[the low half], and returns an
 * UML_GADGET_SVE_LIBRARY_* outcome; a memory fault is in the TLB. */
extern "C" [[gnu::noinline]] std::uint32_t uml_arm64_gadget_sve_execute(
	uml::cpu::arm64::CpuState *cpu, uml::memory::Tlb *tlb,
	std::uint64_t operand) noexcept
{
	namespace detail = uml::cpu::arm64::detail;
	const auto word = static_cast<std::uint32_t>(operand >> 32);
	const auto index = static_cast<std::size_t>(operand & 0xffff'ffffU);
	if (cpu == nullptr || tlb == nullptr ||
	    index >= detail::sme_encodings.size())
		return UML_GADGET_SVE_LIBRARY_FATAL;
	switch (uml::cpu::arm64::gadgets::execute_sve_word(*cpu, *tlb, word,
		detail::sme_encodings[index]).event) {
	case detail::SveEvent::retired:
		return UML_GADGET_SVE_LIBRARY_RETIRED;
	case detail::SveEvent::memory_fault:
		return UML_GADGET_SVE_LIBRARY_FAULT;
	case detail::SveEvent::undefined:
		return UML_GADGET_SVE_LIBRARY_UNDEFINED;
	case detail::SveEvent::floating_point_trap:
		/* While a trap enable is set the executor runs each SVE word
		 * through the library itself; with them clear nothing traps. */
	case detail::SveEvent::fatal:
		break;
	}
	return UML_GADGET_SVE_LIBRARY_FATAL;
}
