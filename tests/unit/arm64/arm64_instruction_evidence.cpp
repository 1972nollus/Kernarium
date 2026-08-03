// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_instruction_evidence.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

namespace uml::emulator::tests {
namespace {

struct EvidenceKey final {
  std::string source;
  std::uint_least32_t line{};
  std::uint32_t opcode_bucket{};
  std::uint16_t alias_shape{};
  bool invalid{};
  std::string stable_id;

  auto operator<=>(const EvidenceKey &) const = default;
};

struct EvidenceRecord final {
  std::string case_name;
  std::uint32_t representative{};
  std::uint64_t encoding_hash{1469598103934665603ULL};
  std::size_t variants{};
  bool interpreter{};
  bool gadgets{};
  bool jit{};
  bool jit_skipped_host_feature{};
  bool jit_unavailable{};
};

[[nodiscard]] std::string tsv_text(std::string_view value) {
  std::string result{value};
  for (auto &character : result)
    if (character == '\t' || character == '\r' || character == '\n')
      character = ' ';
  return result;
}

[[nodiscard]] std::string stable_ids_in(std::string_view case_name) {
  std::string result;
  for (std::size_t position = 0; position < case_name.size(); ++position) {
    for (const auto prefix : {std::string_view{"BASE-"},
                              std::string_view{"FPSIMD-"}}) {
      if (!case_name.substr(position).starts_with(prefix))
        continue;
      const auto digits = position + prefix.size();
      if (digits + 4 > case_name.size() ||
          case_name.substr(digits, 4).find_first_not_of("0123456789") !=
              std::string_view::npos)
        continue;
      if (!result.empty())
        result.push_back(' ');
      result.append(case_name.substr(position, prefix.size() + 4));
      position += prefix.size() + 3;
      break;
    }
  }
  return result;
}

class InstructionEvidenceRecorder final {
public:
  ~InstructionEvidenceRecorder() {
    const auto *path = std::getenv("UML_INSTRUCTION_EVIDENCE");
    if (path == nullptr || path[0] == '\0')
      return;
    auto *output = std::fopen(path, "w");
    if (output == nullptr) {
      std::fprintf(stderr, "cannot write instruction evidence: %s\n", path);
      std::abort();
    }
    const auto *test = std::getenv("UML_INSTRUCTION_EVIDENCE_TEST");
    const auto test_name = tsv_text(test == nullptr ? "unknown" : test);
    std::fprintf(output,
                 "ctest\tcase\tsource\tline\trepresentative\topcode_bucket\t"
                 "alias_shape\tvariants\tencoding_hash\tinvalid\tinterpreter\t"
                 "gadgets\tjit\tjit_skip\n");
    for (const auto &[key, record] : records_) {
      const char *jit_skip = record.jit ? "-" :
          record.jit_skipped_host_feature ? "host-feature" :
          record.jit_unavailable ? "unavailable" : "missing";
      const auto case_name = tsv_text(record.case_name);
      const auto source = tsv_text(key.source);
      std::fprintf(output,
                   "%s\t%s\t%s\t%u\t%08x\t%08x\t%04x\t%zu\t%016llx\t"
                   "%u\t%u\t%u\t%u\t%s\n",
                   test_name.c_str(), case_name.c_str(), source.c_str(),
                   static_cast<unsigned>(key.line), record.representative,
                   key.opcode_bucket, key.alias_shape, record.variants,
                   static_cast<unsigned long long>(record.encoding_hash),
                   key.invalid ? 1U : 0U, record.interpreter ? 1U : 0U,
                   record.gadgets ? 1U : 0U, record.jit ? 1U : 0U,
                   jit_skip);
    }
    if (std::fclose(output) != 0) {
      std::fprintf(stderr, "cannot close instruction evidence: %s\n", path);
      std::abort();
    }
  }

  void record(std::string_view case_name, std::uint32_t instruction,
              bool invalid, const InstructionEngineEvidence engines,
              const std::source_location &location) {
    /* Remove operand-heavy fields while retaining the fixed opcode space.
     * Source line and validity keep independent variant generators separate. */
    constexpr std::uint32_t opcode_bucket_mask = 0xffe0'fc00U;
    const auto rd = instruction & 31U;
    const auto rn = (instruction >> 5U) & 31U;
    const auto ra = (instruction >> 10U) & 31U;
    const auto rm = (instruction >> 16U) & 31U;
    /* Keep the architecturally special register-31 shapes separate.  Other
     * register equalities remain covered by variants/encoding_hash; making
     * every equality combination a key inflated the corpus without adding a
     * distinct architectural interpretation. */
    const auto alias_shape = static_cast<std::uint16_t>(
        (rd == 31U ? 1U << 0U : 0U) |
        (rn == 31U ? 1U << 1U : 0U) |
        (ra == 31U ? 1U << 2U : 0U) |
        (rm == 31U ? 1U << 3U : 0U));
    EvidenceKey key{
        .source = location.file_name(),
        .line = location.line(),
        .opcode_bucket = instruction & opcode_bucket_mask,
        .alias_shape = alias_shape,
        .invalid = invalid,
        .stable_id = stable_ids_in(case_name),
    };
    auto [position, inserted] = records_.try_emplace(std::move(key));
    auto &record = position->second;
    if (inserted) {
      record.case_name = case_name;
      record.representative = instruction;
    }
    ++record.variants;
    record.encoding_hash ^= instruction;
    record.encoding_hash *= 1099511628211ULL;
    record.interpreter = record.interpreter || engines.interpreter;
    record.gadgets = record.gadgets || engines.gadgets;
    record.jit = record.jit ||
        engines.jit == InstructionJitEvidence::executed;
    record.jit_skipped_host_feature = record.jit_skipped_host_feature ||
        engines.jit == InstructionJitEvidence::skipped_host_feature;
    record.jit_unavailable = record.jit_unavailable ||
        engines.jit == InstructionJitEvidence::unavailable;
  }

private:
  std::map<EvidenceKey, EvidenceRecord> records_;
};

} // namespace

void record_instruction_evidence(
    const std::string_view case_name, const std::uint32_t instruction,
    const bool invalid, const InstructionEngineEvidence engines,
    const std::source_location &location) {
  static InstructionEvidenceRecorder recorder;
  recorder.record(case_name, instruction, invalid, engines, location);
}

} // namespace uml::emulator::tests
