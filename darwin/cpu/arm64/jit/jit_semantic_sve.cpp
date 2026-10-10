// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The JIT's C++ side of non-streaming SVE: the words no inline lowering
 * covers run through the engine-neutral semantic library
 * (cpu/arm64/detail/sve_semantics.hpp) on the register file. Generated code
 * reaches it as a terminal helper, whose spill has filled CpuState; the
 * executor's trapped-exception mode reaches the same function directly, so
 * the library alone decides a floating-point form's trap and the flags it
 * commits (detail::decide_floating_exception).
 */
#include "jit_semantic_memory.hpp"
#include "jit_semantic_reservation.hpp"
#include "jit_semantic_sve.hpp"
#include "jit_sve_support.hpp"

#include "cpu/arm64/fp_environment.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <unistd.h>

namespace uml::cpu::arm64 {

namespace {

/* Guest memory through the executor's TLB. The library forms every
 * address untagged, so the plain translation applies; a miss there records
 * its fault in the TLB, which a first-fault or non-fault load's probe may
 * leave behind, and execute_sve_word replaces that record with the
 * library's own whenever the instruction faults. A store invalidates the
 * software reservations of the host bytes it wrote as it writes them. */
class JitSveMemory final : public detail::SveMemory {
public:
  explicit JitSveMemory(Tlb &tlb) noexcept : tlb_{tlb} {}

  std::byte *translate(std::uint64_t address,
                       ::uml::memory::MemoryAccess access) noexcept override {
    return tlb_.translate(address, ::uml::memory::translated_access(access));
  }

  void stored(std::byte *host, std::size_t size) noexcept override {
    const JitStoreRange range{reinterpret_cast<std::uintptr_t>(host), size};
    jit::semantic::invalidate_store_ranges(std::span{&range, 1});
  }

private:
  Tlb &tlb_;
};

} // namespace

namespace jit::semantic {

namespace {

/* The words run through the library per table row, and the last of them,
 * while counting. */
std::array<std::atomic<std::uint64_t>, detail::sme_encodings.size()>
    library_exits{};
std::array<std::atomic<std::uint32_t>, detail::sme_encodings.size()>
    library_exit_words{};
std::atomic<bool> counting{false};

/* The translations of each row, and those that ended in the library. */
std::array<std::atomic<std::uint64_t>, detail::sme_encodings.size()> translated{};
std::array<std::atomic<std::uint64_t>, detail::sme_encodings.size()>
    translated_to_library{};

/*
 * DARWIN_UML_SVE_LIBRARY_EXITS names the file that receives the report
 * (jit_semantic_sve.hpp): the process counts from its start and writes it as
 * it ends and again as each executor retires, since the emulator leaves
 * through _Exit, which runs no exit handler. The report is cumulative, so
 * the last write is the complete one. A path of another process is not
 * appended to: give each process its own.
 */
char report_path[1024];
std::mutex report_mutex;

void write_report_from_environment() noexcept {
  if (report_path[0] == '\0')
    return;
  try {
    const std::lock_guard guard{report_mutex};
    (void)jit::semantic::write_sve_library_report(report_path);
  } catch (...) {
  }
}

/* Whether a JIT executor has retired: a report with nothing in it is then a
 * result. A process that ran none, as one running only the gadget engine,
 * has no result of its own, and that engine writes the same file in the same
 * format: this one leaves it alone. */
std::atomic<bool> executor_retired{false};

bool any_translation() noexcept {
  for (const auto &count : translated)
    if (count.load(std::memory_order_relaxed) != 0U)
      return true;
  return false;
}

void write_report_at_exit() noexcept {
  if (!executor_retired.load(std::memory_order_relaxed) &&
      sve_library_exit_total() == 0U && !any_translation())
    return;
  write_report_from_environment();
}

struct ReportFromEnvironment final {
  ReportFromEnvironment() noexcept {
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

void count_sve_library_exits(bool enable) noexcept {
  counting.store(enable, std::memory_order_relaxed);
}

std::uint64_t sve_library_exits(std::size_t row) noexcept {
  return row < library_exits.size()
             ? library_exits[row].load(std::memory_order_relaxed)
             : 0U;
}

std::uint32_t sve_library_exit_word(std::size_t row) noexcept {
  return row < library_exit_words.size()
             ? library_exit_words[row].load(std::memory_order_relaxed)
             : 0U;
}

std::uint64_t sve_library_exit_total() noexcept {
  std::uint64_t total = 0;
  for (const auto &count : library_exits)
    total += count.load(std::memory_order_relaxed);
  return total;
}

void reset_sve_library_exits() noexcept {
  for (auto &count : library_exits)
    count.store(0, std::memory_order_relaxed);
  for (auto &word : library_exit_words)
    word.store(0, std::memory_order_relaxed);
  for (auto &count : translated)
    count.store(0, std::memory_order_relaxed);
  for (auto &count : translated_to_library)
    count.store(0, std::memory_order_relaxed);
}

void note_sve_translation(std::size_t row, bool library) noexcept {
  if (row >= translated.size() || !counting.load(std::memory_order_relaxed))
    return;
  translated[row].fetch_add(1, std::memory_order_relaxed);
  if (library)
    translated_to_library[row].fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t sve_translated(std::size_t row) noexcept {
  return row < translated.size()
             ? translated[row].load(std::memory_order_relaxed)
             : 0U;
}

std::uint64_t sve_translated_to_library(std::size_t row) noexcept {
  return row < translated_to_library.size()
             ? translated_to_library[row].load(std::memory_order_relaxed)
             : 0U;
}

bool write_sve_library_report(const char *path) noexcept {
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
      const Line line{row, sve_translated(row), sve_translated_to_library(row),
                      sve_library_exits(row)};
      if (line.translated != 0U || line.exits != 0U)
        lines.push_back(line);
    }
    std::ranges::sort(lines, [](const Line &a, const Line &b) {
      return a.exits != b.exits             ? a.exits > b.exits
             : a.translated != b.translated ? a.translated > b.translated
                                            : a.row < b.row;
    });
  } catch (...) {
    std::fclose(file);
    return false;
  }
  std::fprintf(file,
               "# SVE library exits: total %llu\n"
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

void write_sve_library_report_from_environment() noexcept {
  executor_retired.store(true, std::memory_order_relaxed);
  write_report_from_environment();
}

sve_detail::SveResult execute_sve_word(CpuState &cpu, Tlb &tlb,
                                   std::uint32_t word,
                                   const sve_detail::SmeEncoding &row) noexcept {
  if (counting.load(std::memory_order_relaxed)) {
    const auto index =
        static_cast<std::size_t>(&row - detail::sme_encodings.data());
    if (index < library_exits.size()) {
      library_exits[index].fetch_add(1, std::memory_order_relaxed);
      library_exit_words[index].store(word, std::memory_order_relaxed);
    }
  }
  JitSveMemory memory{tlb};
  const auto result = sve_detail::execute_sve(cpu, word, row, memory);
  if (result.event == sve_detail::SveEvent::memory_fault) {
    tlb.fault_address = result.fault_address;
    tlb.fault_access = ::uml::memory::translated_access(result.access);
    tlb.fault_alignment = result.alignment ? 1U : 0U;
  }
  return result;
}

std::optional<SveWordRun> run_sve_word(
    CpuState &cpu, Tlb &tlb, std::uint32_t word,
    const ::uml::cpu::HostCapabilities &host) noexcept {
  const auto *row = sve_detail::decode_sve_non_streaming(
      word, sve_detail::implemented_sve_features(host));
  if (row == nullptr ||
      ((row->mode & sve_detail::sme_mode_za) != 0U &&
       (cpu.svcr & svcr_za) == 0U) ||
      !sve_detail::sve_semantics_covers(*row))
    return std::nullopt;
  if (jit::sve_word_reserved(*row, word))
    return SveWordRun{Event::illegal_instruction, 0, 0};
  restore_native_fp_environment(cpu);
  const auto result = execute_sve_word(cpu, tlb, word, *row);
  switch (result.event) {
  case sve_detail::SveEvent::retired:
    cpu.pc += sizeof(std::uint32_t);
    return SveWordRun{Event::continued, 1, 0};
  case sve_detail::SveEvent::memory_fault:
    return SveWordRun{Event::memory_fault, 0, 0};
  case sve_detail::SveEvent::floating_point_trap:
    return SveWordRun{Event::floating_point_trap, 0, result.syndrome};
  case sve_detail::SveEvent::undefined:
    return SveWordRun{Event::illegal_instruction, 0, 0};
  case sve_detail::SveEvent::fatal:
    break;
  }
  return SveWordRun{Event::fatal, 0, 0};
}

} // namespace jit::semantic

/* The slow path of every SM=0 SVE word the translation does not lower: a
 * terminal helper, so the block's spill has filled CpuState. A trapped
 * floating-point form never gets here, since the executor runs each word
 * through the library itself while an FPCR trap enable is set. */
extern "C" std::uint64_t
uml_arm64_jit_execute_sve(CpuState *cpu, Tlb *tlb,
                          std::uint32_t instruction) noexcept {
  const auto *row = detail::find_sme_encoding(instruction);
  if (cpu == nullptr || tlb == nullptr || row == nullptr ||
      !detail::sve_semantics_covers(*row))
    return pack_jit_result(Event::fatal, 0);
  restore_native_fp_environment(*cpu);
  switch (jit::semantic::execute_sve_word(*cpu, *tlb, instruction, *row)
              .event) {
  case detail::SveEvent::retired:
    cpu->pc += sizeof(std::uint32_t);
    return pack_jit_result(Event::continued, 1);
  case detail::SveEvent::memory_fault:
    return pack_jit_result(Event::memory_fault, 0);
  case detail::SveEvent::undefined:
    return pack_jit_result(Event::illegal_instruction, 0);
  case detail::SveEvent::floating_point_trap:
  case detail::SveEvent::fatal:
    break;
  }
  return pack_jit_result(Event::fatal, 0);
}

} // namespace uml::cpu::arm64
