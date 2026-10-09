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

#include "cpu/arm64/sme_profile.hpp"
#include "cpu/arm64/sve_profile.hpp"

#include <array>
#include <atomic>
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
	    detail::sve_reserved_encoding(word, *row))
		return {.kind = SveWord::undefined, .row = row};
	return {.kind = SveWord::implemented, .row = row};
}

namespace {

/* The words run through the library per table row, while counting. */
std::array<std::atomic<std::uint64_t>, detail::sme_encodings.size()>
	library_exits{};
std::atomic<bool> counting{false};

} // namespace

void count_sve_library_exits(bool enable) noexcept
{
	counting.store(enable, std::memory_order_relaxed);
}

std::uint64_t sve_library_exits(std::size_t row) noexcept
{
	return row < library_exits.size() ?
		library_exits[row].load(std::memory_order_relaxed) : 0U;
}

std::uint64_t sve_library_exit_total() noexcept
{
	std::uint64_t total = 0;
	for (const auto &count : library_exits)
		total += count.load(std::memory_order_relaxed);
	return total;
}

void reset_sve_library_exits() noexcept
{
	for (auto &count : library_exits)
		count.store(0, std::memory_order_relaxed);
}

detail::SveResult execute_sve_word(CpuState &cpu, memory::Tlb &tlb,
	std::uint32_t word, const detail::SmeEncoding &row) noexcept
{
	if (counting.load(std::memory_order_relaxed))
		library_exits[static_cast<std::size_t>(
			&row - detail::sme_encodings.data())]
			.fetch_add(1, std::memory_order_relaxed);
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
