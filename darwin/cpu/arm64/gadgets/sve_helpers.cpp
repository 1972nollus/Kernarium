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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

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

/* The words run through the library per table row, while counting, and the
 * translations that sent a word there or elsewhere. */
std::array<std::atomic<std::uint64_t>, detail::sme_encodings.size()>
	library_exits{};
std::array<std::atomic<std::uint64_t>, detail::sme_encodings.size()>
	translated{};
std::array<std::atomic<std::uint64_t>, detail::sme_encodings.size()>
	translated_to_library{};
std::atomic<bool> counting{false};

/* The report DARWIN_UML_SVE_LIBRARY_EXITS asks for: counting from the
 * process's start, a report at its exit. */
char report_path[1024];

std::mutex report_mutex;

void write_report() noexcept
{
	try {
		const std::lock_guard guard{report_mutex};
		write_sve_library_report(report_path);
	} catch (...) {
	}
}

void write_report_at_exit() noexcept
{
	write_report();
}

struct ReportFromEnvironment final {
	ReportFromEnvironment() noexcept
	{
		const char *path = std::getenv("DARWIN_UML_SVE_LIBRARY_EXITS");
		if (path == nullptr || *path == '\0' ||
		    std::strlen(path) >= sizeof report_path)
			return;
		std::strcpy(report_path, path);
		counting.store(true, std::memory_order_relaxed);
		std::atexit(write_report_at_exit);
	}
};

const ReportFromEnvironment report_from_environment;

} // namespace

void write_sve_library_report_from_environment() noexcept
{
	if (report_path[0] != '\0')
		write_report();
}

void note_sve_translation(std::size_t row, bool library) noexcept
{
	if (row >= translated.size() ||
	    !counting.load(std::memory_order_relaxed))
		return;
	translated[row].fetch_add(1, std::memory_order_relaxed);
	if (library)
		translated_to_library[row].fetch_add(1,
			std::memory_order_relaxed);
}

std::uint64_t sve_translated(std::size_t row) noexcept
{
	return row < translated.size() ?
		translated[row].load(std::memory_order_relaxed) : 0U;
}

std::uint64_t sve_translated_to_library(std::size_t row) noexcept
{
	return row < translated_to_library.size() ?
		translated_to_library[row].load(std::memory_order_relaxed) : 0U;
}

bool write_sve_library_report(const char *path) noexcept
{
	struct Line final {
		std::size_t row;
		std::uint64_t translated;
		std::uint64_t to_library;
		std::uint64_t exits;
	};
	std::FILE *file = std::fopen(path, "w");
	if (file == nullptr)
		return false;
	std::vector<Line> lines;
	try {
		for (std::size_t row = 0; row < library_exits.size(); ++row) {
			const Line line{row, sve_translated(row),
				sve_translated_to_library(row),
				sve_library_exits(row)};
			if (line.translated != 0U || line.exits != 0U)
				lines.push_back(line);
		}
		std::ranges::sort(lines, [](const Line &a, const Line &b) {
			return a.exits != b.exits ? a.exits > b.exits :
				a.translated != b.translated ?
				a.translated > b.translated : a.row < b.row;
		});
	} catch (...) {
		std::fclose(file);
		return false;
	}
	std::fprintf(file, "# SVE library exits: total %llu\n"
		"# form\trow\tmask\tvalue\ttranslated\t"
		"translated_to_library\texits\n",
		static_cast<unsigned long long>(sve_library_exit_total()));
	for (const auto &line : lines) {
		const auto &row = detail::sme_encodings[line.row];
		std::fprintf(file, "SVE-%04u\t%zu\t%08x\t%08x\t%llu\t%llu\t%llu\n",
			static_cast<unsigned>(row.form & 0x7fffU), line.row,
			static_cast<unsigned>(row.mask),
			static_cast<unsigned>(row.value),
			static_cast<unsigned long long>(line.translated),
			static_cast<unsigned long long>(line.to_library),
			static_cast<unsigned long long>(line.exits));
	}
	return std::fclose(file) == 0;
}

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
	for (auto &count : translated)
		count.store(0, std::memory_order_relaxed);
	for (auto &count : translated_to_library)
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
