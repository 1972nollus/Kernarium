// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uml::emulator::tests::corpus_export {

/*
 * Instruction-corpus export mode.
 *
 * UML_ARM64_CORPUS_EXPORT=<directory> turns every harness producer into a
 * two-pass exporter. The producer name is the CTest name that the unit-test
 * wrapper supplies; tools/arm64-tests/export-unit-corpus.py drives both passes
 * over every producer of a build tree.
 *
 * UML_ARM64_CORPUS_EXPORT_PASS=count classifies each case without running an
 * engine and records the size of every equivalence class and how many of its
 * cases the boundary rule below selects.
 * UML_ARM64_CORPUS_EXPORT_PASS=sample repeats the deterministic case stream,
 * selects the first and last case of each class plus a fixed-seed uniform
 * sample of at most UML_ARM64_CORPUS_K cases (UML_ARM64_CORPUS_SEED), and
 * every case that gives an operand field of one of its program words a
 * boundary value that no earlier such case of its class gave it. It verifies
 * each selected case through every engine lane and appends it to a
 * compressed export stream. Unselected cases run no engine in either pass.
 *
 * UML_ARM64_CORPUS_INCLUDE=<file> additionally forces cases into the sample
 * pass. Each line other than blank lines and # comments is one tab-separated
 * directive:
 *
 *   case  <producer> <ordinal>
 *   word  <mask> <value> [<producer> or * [<per-class limit>]]
 *
 * A case directive selects one check of one producer. A word directive
 * selects every case with a program word w for which (w & mask) == value.
 * Check ordinals number the check() and check_program() calls of one process
 * from 1; a harness abort reports the ordinal of the failing check, and the
 * sample pass lists the ordinal of every exported case.
 */

struct ExportRegion final {
  std::uint64_t address{};
  std::vector<std::byte> initial;
  std::vector<std::byte> expected;
  std::vector<AllocationTag> initial_tags;
  std::vector<AllocationTag> expected_tags;
  bool readable{true};
  bool writable{true};
  bool executable{};
  bool guest_code{};
  bool guest_writable{true};
};

/* A deep copy of one ProgramCase. Every span in view() refers to this object. */
class ExportCase final {
public:
  ExportCase(const ProgramCase &source);

  ExportCase(const ExportCase &) = delete;
  ExportCase &operator=(const ExportCase &) = delete;

  [[nodiscard]] ProgramCase view() const;

  std::string name;
  std::vector<ProgramWord> code;
  std::size_t entry_offset{};
  arm64::CpuState initial;
  arm64::CpuState expected;
  ExpectedEvent event{ExpectedEvent::system_call};
  NativeRequirements native;
  MemoryFaultExpectation fault{};
  bool compare_fault{};
  std::uint32_t syndrome{};
  bool invalid_encoding{};
  std::vector<ExportRegion> memory;
  ArchitectureValidator validator{};
  std::string validator_relation_id;
  std::optional<std::size_t> expected_instructions{};
  std::optional<InstructionCountPattern> expected_instruction_pattern{};

private:
  std::vector<MemoryRegion> region_views_;
};

/* Minimization levels, from the most compact to the unmodified state. */
enum class Minimization : std::uint8_t {
  full = 0,
  narrow = 1,
  broad = 2,
};

/* Canonical register fill shared with the corpus runner and builder. */
[[nodiscard]] std::uint64_t fill_register(unsigned index) noexcept;
[[nodiscard]] std::byte fill_vector_byte(unsigned index,
                                         unsigned byte) noexcept;

/* Replaces every register outside the candidate set with the canonical fill.
 * Unchanged registers are rewritten in both states; a register the program
 * writes keeps its expected value. Returns false when nothing changed. */
[[nodiscard]] bool minimize(ExportCase &test_case, Minimization level);

struct Selection final {
  bool selected{};
  bool sampled{};
  bool covering{};
  bool forced{};
  std::uint64_t ordinal{};
  std::uint64_t index{};
  std::uint64_t size{};
  std::string class_key;
  std::string site;
};

struct ExportResult final {
  Minimization minimization{Minimization::full};
  std::optional<std::size_t> interpreter_instructions;
  bool interpreter{};
  bool gadgets{};
  bool jit{};
  bool jit_skipped_host_feature{};
};

class Exporter final {
public:
  /* Returns the process exporter, or nullptr outside export mode. */
  [[nodiscard]] static Exporter *active();

  /* Records one exportable case and reports whether it was selected. */
  [[nodiscard]] Selection observe(const ProgramCase &test_case,
                                  const EngineHarnessOptions &options,
                                  const std::source_location &location,
                                  std::uint64_t ordinal);
  /* Counts one case that must stay in its focused producer test. */
  void exclude(const ProgramCase &test_case, std::string_view reason,
               const std::source_location &location, std::uint64_t ordinal);
  void write(const ExportCase &test_case, const Selection &selection,
             const EngineHarnessOptions &options, const ExportResult &result);

  ~Exporter();

private:
  Exporter();
  class State;
  State *state_{};
};

[[nodiscard]] std::uint32_t option_mask(
    const EngineHarnessOptions &options) noexcept;

} // namespace uml::emulator::tests::corpus_export
