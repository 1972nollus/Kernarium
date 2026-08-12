// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_instruction_evidence.hpp"

#include <charconv>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace uml::emulator::tests {
namespace {

struct EvidenceKey final {
  std::string stable_id;
  std::string section;
  std::string architectural_form;
  std::string alias_group;
  std::string ctest;
  std::string source;
  std::string case_name;
  std::uint_least32_t line{};
  std::uint32_t instruction{};
  std::uint32_t guest_page_size{};
  bool invalid{};
  InstructionEngineResult interpreter{InstructionEngineResult::unavailable};
  InstructionEngineResult gadgets{InstructionEngineResult::unavailable};
  InstructionEngineResult jit{InstructionEngineResult::unavailable};
  std::string native_required_features;
  InstructionNativeScope native_requirement_scope{
      InstructionNativeScope::instruction};
  std::string native_observed_features;
  std::string native_missing_features;
  std::string expected_event;
  std::string expected_retirement;
  std::string expected_state_oracle;
  std::uint64_t initial_state_digest{};
  std::uint64_t initial_memory_digest{};
  std::uint64_t expected_state_digest{};
  std::uint64_t expected_memory_digest{};
  std::string expected_fault;
  std::uint32_t expected_syndrome{};

  [[nodiscard]] bool operator<(const EvidenceKey &other) const noexcept {
    return std::tie(stable_id, section, architectural_form, alias_group, ctest,
                    source, case_name, line, instruction, guest_page_size,
                    invalid, interpreter, gadgets, jit, native_required_features,
                    native_requirement_scope,
                    native_observed_features, native_missing_features,
                    expected_event,
                    expected_retirement, expected_state_oracle,
                    initial_state_digest, initial_memory_digest,
                    expected_state_digest, expected_memory_digest,
                    expected_fault, expected_syndrome) <
           std::tie(other.stable_id, other.section, other.architectural_form,
                    other.alias_group, other.ctest, other.source,
                    other.case_name, other.line, other.instruction,
                    other.guest_page_size, other.invalid,
                    other.interpreter, other.gadgets, other.jit,
                    other.native_required_features,
                    other.native_requirement_scope,
                    other.native_observed_features,
                    other.native_missing_features, other.expected_event,
                    other.expected_retirement, other.expected_state_oracle,
                    other.initial_state_digest,
                    other.initial_memory_digest,
                    other.expected_state_digest, other.expected_memory_digest,
                    other.expected_fault, other.expected_syndrome);
  }
};

struct EvidenceBinding final {
  std::string stable_id;
  std::string section;
  std::string architectural_form;
  std::string case_name;
  std::uint32_t instruction{};
  std::string alias_group;
};

[[nodiscard]] std::string tsv_text(std::string_view value) {
  std::string result{value};
  for (auto &character : result)
    if (character == '\t' || character == '\r' || character == '\n')
      character = ' ';
  return result;
}

[[nodiscard]] const char *result_name(InstructionEngineResult result) {
  switch (result) {
  case InstructionEngineResult::unavailable:
    return "unavailable";
  case InstructionEngineResult::skipped_host_feature:
    return "skipped-host-feature";
  case InstructionEngineResult::executed_not_retired:
    return "executed-not-retired";
  case InstructionEngineResult::executed_retired:
    return "executed-retired";
  }
  return "invalid";
}

[[nodiscard]] const char *scope_name(InstructionNativeScope scope) {
  switch (scope) {
  case InstructionNativeScope::instruction:
    return "instruction";
  case InstructionNativeScope::program:
    return "program";
  }
  return "invalid";
}

[[nodiscard]] std::string feature_names(
    const ::uml::cpu::ArmFeatureSet &features) {
  if (features.none())
    return "baseline";
  std::string result;
  for (std::size_t index = 0; index < ::uml::cpu::arm_feature_count; ++index) {
    if (!features.test(index))
      continue;
    if (!result.empty())
      result += '+';
    result += ::uml::cpu::arm_feature_tokens[index];
  }
  return result;
}

[[nodiscard]] std::string observed_feature_names(
    const ::uml::cpu::ArmFeatureSet &required,
    const ::uml::cpu::ArmFeatureSet &observed) {
  if (required.none())
    return "baseline=1";
  std::string result;
  for (std::size_t index = 0; index < ::uml::cpu::arm_feature_count; ++index) {
    if (!required.test(index))
      continue;
    if (!result.empty())
      result += '+';
    result += ::uml::cpu::arm_feature_tokens[index];
    result += observed.test(index) ? "=1" : "=0";
  }
  return result;
}

[[nodiscard]] std::string missing_feature_names(
    const ::uml::cpu::ArmFeatureSet &required,
    const ::uml::cpu::ArmFeatureSet &observed) {
  std::string result;
  for (std::size_t index = 0; index < ::uml::cpu::arm_feature_count; ++index) {
    if (!required.test(index) || observed.test(index))
      continue;
    if (!result.empty())
      result += '+';
    result += ::uml::cpu::arm_feature_tokens[index];
  }
  return result.empty() ? "none" : result;
}

[[noreturn]] void configuration_error(const std::string &message) {
  std::fprintf(stderr, "invalid instruction evidence configuration: %s\n",
               message.c_str());
  std::abort();
}

[[nodiscard]] std::string canonical_output_path(const char *path) {
  const std::filesystem::path requested{path};
  std::error_code error;
  const auto parent = std::filesystem::canonical(requested.parent_path(), error);
  if (error || requested.filename().empty())
    configuration_error("evidence output parent is not canonical");
  return (parent / requested.filename()).string();
}

[[nodiscard]] std::string lock_marker(std::string_view test,
                                      std::string_view evidence_path) {
  return "v1\n" + std::string{test} + "\n" + std::string{evidence_path};
}

struct StagedTextOutput final {
  std::string path;
  std::string temporary;
  std::FILE *stream{};
};

[[nodiscard]] StagedTextOutput open_staged_text_output(
    const std::string &path) {
  StagedTextOutput output{.path = path,
                          .temporary = path + ".producer.tmp"};
  if (::unlink(output.temporary.c_str()) != 0 && errno != ENOENT)
    configuration_error("cannot remove stale evidence staging file");
  auto flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  const auto descriptor = ::open(output.temporary.c_str(), flags, 0666);
  if (descriptor < 0)
    configuration_error("cannot create evidence staging file");
  output.stream = ::fdopen(descriptor, "w");
  if (output.stream == nullptr) {
    ::close(descriptor);
    configuration_error("cannot open evidence staging stream");
  }
  return output;
}

void close_staged_text_output(StagedTextOutput &output) {
  if (std::fflush(output.stream) != 0 || std::ferror(output.stream) != 0)
    configuration_error("cannot flush evidence staging stream");
  if (std::fclose(output.stream) != 0)
    configuration_error("cannot close evidence staging stream");
  output.stream = nullptr;
}

void publish_staged_text_output(const StagedTextOutput &output) {
  if (::rename(output.temporary.c_str(), output.path.c_str()) != 0)
    configuration_error("cannot publish evidence output");
}

[[nodiscard]] std::vector<std::string> split_tsv(const std::string &line) {
  std::vector<std::string> fields;
  std::size_t begin = 0;
  for (;;) {
    const auto tab = line.find('\t', begin);
    fields.emplace_back(line.substr(begin, tab - begin));
    if (tab == std::string::npos)
      return fields;
    begin = tab + 1;
  }
}

[[nodiscard]] std::uint32_t parse_encoding(std::string_view text,
                                           std::size_t line_number) {
  std::uint32_t instruction = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), instruction, 16);
  if (text.size() != 8 || error != std::errc{} ||
      end != text.data() + text.size())
    configuration_error("invalid encoding on binding line " +
                        std::to_string(line_number));
  return instruction;
}

class InstructionEvidenceRecorder final {
public:
  ~InstructionEvidenceRecorder() {
    if (!configured_ || !enabled_)
      return;
    const auto *path = output_path_.c_str();
    std::error_code directory_error;
    std::filesystem::create_directories(
        std::filesystem::path{path}.parent_path(), directory_error);
    if (directory_error) {
      std::fprintf(stderr, "cannot create instruction evidence directory: %s\n",
                   directory_error.message().c_str());
      std::abort();
    }
    auto staged_output = open_staged_text_output(path);
    auto *output = staged_output.stream;
    std::fprintf(output,
                 "stable_id\tsection\tarchitectural_form\talias_group\t"
                 "ctest\tcase\tsource\tline\tencoding\tinvalid\t"
                 "guest_page_size\t"
                 "expected_event\texpected_retirement\t"
                 "expected_state_oracle\tinitial_state_digest\t"
                 "initial_memory_digest\texpected_state_digest\t"
                 "expected_memory_digest\texpected_fault\t"
                 "expected_syndrome\t"
                 "native_required_features\tnative_requirement_scope\t"
                 "native_observed_features\t"
                 "native_missing_features\t"
                 "interpreter_result\tgadgets_result\tjit_result\n");
    for (const auto &key : records_) {
      const auto stable_id = tsv_text(key.stable_id);
      const auto section = tsv_text(key.section);
      const auto architectural_form = tsv_text(key.architectural_form);
      const auto alias_group = tsv_text(key.alias_group);
      const auto ctest = tsv_text(key.ctest);
      const auto case_name = tsv_text(key.case_name);
      const auto source = tsv_text(key.source);
      const auto native_required_features =
          tsv_text(key.native_required_features);
      const auto native_observed_features =
          tsv_text(key.native_observed_features);
      const auto native_missing_features =
          tsv_text(key.native_missing_features);
      const auto expected_event = tsv_text(key.expected_event);
      const auto expected_retirement = tsv_text(key.expected_retirement);
      const auto expected_state_oracle = tsv_text(key.expected_state_oracle);
      const auto expected_fault = tsv_text(key.expected_fault);
      std::fprintf(output,
                   "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%u\t%08x\t%u\t%u\t"
                   "%s\t%s\t%s\t%016llx\t%016llx\t%016llx\t%016llx\t"
                   "%s\t%08x\t"
                   "%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
                   stable_id.c_str(), section.c_str(),
                   architectural_form.c_str(), alias_group.c_str(),
                   ctest.c_str(), case_name.c_str(), source.c_str(),
                   static_cast<unsigned>(key.line), key.instruction,
                   key.invalid ? 1U : 0U,
                   key.guest_page_size,
                   expected_event.c_str(), expected_retirement.c_str(),
                   expected_state_oracle.c_str(),
                   static_cast<unsigned long long>(
                       key.initial_state_digest),
                   static_cast<unsigned long long>(
                       key.initial_memory_digest),
                   static_cast<unsigned long long>(
                       key.expected_state_digest),
                   static_cast<unsigned long long>(
                       key.expected_memory_digest),
                   expected_fault.c_str(), key.expected_syndrome,
                   native_required_features.c_str(),
                   scope_name(key.native_requirement_scope),
                   native_observed_features.c_str(),
                   native_missing_features.c_str(), result_name(key.interpreter),
                   result_name(key.gadgets), result_name(key.jit));
    }
    close_staged_text_output(staged_output);

    if (!raw_enabled_) {
      publish_staged_text_output(staged_output);
      return;
    }

    const auto raw_path = std::string{path} + ".raw";
    auto staged_raw_output = open_staged_text_output(raw_path);
    auto *raw_output = staged_raw_output.stream;
    std::fprintf(raw_output,
                 "ctest\tcase\tsource\tline\tencoding\tinvalid\t"
                 "guest_page_size\t"
                 "expected_event\texpected_retirement\t"
                 "expected_state_oracle\tinitial_state_digest\t"
                 "initial_memory_digest\texpected_state_digest\t"
                 "expected_memory_digest\texpected_fault\t"
                 "expected_syndrome\t"
                 "native_required_features\tnative_requirement_scope\t"
                 "native_observed_features\t"
                 "native_missing_features\t"
                 "interpreter_result\tgadgets_result\tjit_result\t"
                 "occurrences\n");
    std::uint64_t raw_invocations = 0;
    for (const auto &[key, occurrences] : raw_records_) {
      const auto ctest = tsv_text(key.ctest);
      const auto case_name = tsv_text(key.case_name);
      const auto source = tsv_text(key.source);
      const auto native_required_features =
          tsv_text(key.native_required_features);
      const auto native_observed_features =
          tsv_text(key.native_observed_features);
      const auto native_missing_features =
          tsv_text(key.native_missing_features);
      const auto expected_event = tsv_text(key.expected_event);
      const auto expected_retirement = tsv_text(key.expected_retirement);
      const auto expected_state_oracle = tsv_text(key.expected_state_oracle);
      const auto expected_fault = tsv_text(key.expected_fault);
      std::fprintf(raw_output,
                   "%s\t%s\t%s\t%u\t%08x\t%u\t%u\t"
                   "%s\t%s\t%s\t%016llx\t%016llx\t%016llx\t%016llx\t"
                   "%s\t%08x\t"
                   "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%llu\n",
                   ctest.c_str(), case_name.c_str(), source.c_str(),
                   static_cast<unsigned>(key.line), key.instruction,
                   key.invalid ? 1U : 0U, key.guest_page_size,
                   expected_event.c_str(), expected_retirement.c_str(),
                   expected_state_oracle.c_str(),
                   static_cast<unsigned long long>(key.initial_state_digest),
                   static_cast<unsigned long long>(key.initial_memory_digest),
                   static_cast<unsigned long long>(key.expected_state_digest),
                   static_cast<unsigned long long>(key.expected_memory_digest),
                   expected_fault.c_str(), key.expected_syndrome,
                   native_required_features.c_str(),
                   scope_name(key.native_requirement_scope),
                   native_observed_features.c_str(),
                   native_missing_features.c_str(), result_name(key.interpreter),
                   result_name(key.gadgets), result_name(key.jit),
                   static_cast<unsigned long long>(occurrences));
      raw_invocations += occurrences;
    }
    close_staged_text_output(staged_raw_output);
    publish_staged_text_output(staged_output);
    publish_staged_text_output(staged_raw_output);
    std::fprintf(stderr,
                 "raw instruction evidence: %llu invocations, %zu unique "
                 "contracts, %zu representative bindings\n",
                 static_cast<unsigned long long>(raw_invocations),
                 raw_records_.size(), records_.size());
  }

  void record(std::string_view case_name, std::uint32_t instruction,
              bool invalid, const InstructionEngineEvidence engines,
              const InstructionNativeEvidence native,
              const InstructionNativeScope native_scope,
              const InstructionExpectationEvidence expectation,
              const std::source_location &location) {
    configure();
    if (!enabled_)
      return;
    EvidenceKey raw{
        .ctest = test_name_,
        .source = location.file_name(),
        .case_name = std::string{case_name},
        .line = location.line(),
        .instruction = instruction,
        .guest_page_size = DARWIN_UML_GUEST_PAGE_SIZE,
        .invalid = invalid,
        .interpreter = engines.interpreter,
        .gadgets = engines.gadgets,
        .jit = engines.jit,
        .native_required_features = feature_names(native.required),
        .native_requirement_scope = native_scope,
        .native_observed_features =
            observed_feature_names(native.required, native.observed),
        .native_missing_features =
            missing_feature_names(native.required, native.observed),
        .expected_event = std::string{expectation.event},
        .expected_retirement = std::string{expectation.retirement},
        .expected_state_oracle = std::string{expectation.state_oracle},
        .initial_state_digest = expectation.initial_state_digest,
        .initial_memory_digest = expectation.initial_memory_digest,
        .expected_state_digest = expectation.state_digest,
        .expected_memory_digest = expectation.memory_digest,
        .expected_fault = std::string{expectation.fault},
        .expected_syndrome = expectation.syndrome,
    };
    if (raw_enabled_)
      ++raw_records_[raw];
    for (const auto &binding : bindings_) {
      if (binding.case_name != case_name ||
          binding.instruction != instruction)
        continue;
      auto key = raw;
      key.stable_id = binding.stable_id;
      key.section = binding.section;
      key.architectural_form = binding.architectural_form;
      key.alias_group = binding.alias_group;
      records_.insert(std::move(key));
    }
  }

private:
  void configure() {
    if (configured_)
      return;
    configured_ = true;
    const auto *output = std::getenv("UML_INSTRUCTION_EVIDENCE");
    if (output == nullptr || output[0] == '\0')
      return;
    const auto *test = std::getenv("UML_INSTRUCTION_EVIDENCE_TEST");
    if (test == nullptr || test[0] == '\0')
      configuration_error("UML_INSTRUCTION_EVIDENCE_TEST is unset");
    test_name_ = test;
    output_path_ = canonical_output_path(output);
    const auto *locked =
        std::getenv("UML_INSTRUCTION_EVIDENCE_LOCKED");
    if (locked == nullptr ||
        std::string_view{locked} != lock_marker(test_name_, output_path_))
      configuration_error("evidence producer runner lock binding differs");
    enabled_ = true;
    const auto *raw = std::getenv("UML_INSTRUCTION_RAW_EVIDENCE");
    raw_enabled_ = raw != nullptr && raw[0] != '\0' && raw[0] != '0';

    const auto *path =
        std::getenv("UML_INSTRUCTION_EVIDENCE_BINDINGS");
    if (path == nullptr || path[0] == '\0')
      configuration_error("UML_INSTRUCTION_EVIDENCE_BINDINGS is unset");
    std::ifstream input{path};
    if (!input)
      configuration_error(std::string{"cannot read binding catalog: "} +
                          path);

    std::string line;
    if (!std::getline(input, line))
      configuration_error("binding catalog is empty");
    const auto header = split_tsv(line);
    const std::vector<std::string> required_header{
        "stable_id", "section", "architectural_form", "ctest",
        "case", "encoding", "alias_group", "native_required_features"};
    if (header != required_header)
      configuration_error("binding catalog has unexpected columns");

    std::size_t line_number = 1;
    while (std::getline(input, line)) {
      ++line_number;
      if (line.empty())
        configuration_error("empty binding line " +
                            std::to_string(line_number));
      const auto fields = split_tsv(line);
      if (fields.size() != header.size())
        configuration_error("wrong field count on binding line " +
                            std::to_string(line_number));
      if (fields[3] != test_name_)
        continue;
      bindings_.push_back({
          .stable_id = fields[0],
          .section = fields[1],
          .architectural_form = fields[2],
          .case_name = fields[4],
          .instruction = parse_encoding(fields[5], line_number),
          .alias_group = fields[6],
      });
    }
    if (!input.eof())
      configuration_error("failed while reading binding catalog");
  }

  bool configured_{};
  bool enabled_{};
  bool raw_enabled_{};
  std::string test_name_;
  std::string output_path_;
  std::vector<EvidenceBinding> bindings_;
  std::map<EvidenceKey, std::uint64_t> raw_records_;
  std::set<EvidenceKey> records_;
};

} // namespace

void record_instruction_evidence(
    const std::string_view case_name, const std::uint32_t instruction,
    const bool invalid, const InstructionEngineEvidence engines,
    const InstructionNativeEvidence native,
    const InstructionNativeScope native_scope,
    const InstructionExpectationEvidence expectation,
    const std::source_location &location) {
  static InstructionEvidenceRecorder recorder;
  recorder.record(case_name, instruction, invalid, engines, native,
                  native_scope,
                  expectation, location);
}

} // namespace uml::emulator::tests
