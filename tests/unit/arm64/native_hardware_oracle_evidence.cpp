// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_hardware_oracle_evidence.hpp"
#include "instruction_evidence_digest.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <utility>

namespace uml::emulator::tests::native_oracle {
namespace {

[[nodiscard]] std::uint8_t initial_tag_at(
    const MemoryRegion &region, const std::size_t offset) noexcept {
  std::uint8_t result{};
  for (const auto tag : region.initial_tags)
    if (tag.offset == offset)
      result = tag.value;
  return result;
}

[[nodiscard]] std::uint64_t
initial_memory_digest(const Request &request) noexcept {
  if (request.memory.empty())
    return instruction_evidence::empty_memory_digest();
  instruction_evidence::Digest digest;
  digest.scalar(request.memory.size());
  for (const auto &region : request.memory) {
    digest.scalar(region.address);
    digest.scalar(region.initial.size());
    digest.boolean(region.readable);
    digest.boolean(region.writable);
    digest.boolean(region.executable);
    digest.boolean(region.guest_code);
    digest.boolean(region.guest_writable);
    digest.bytes(region.initial.data(), region.initial.size());
    for (std::size_t offset = 0; offset < region.initial.size(); offset += 16U)
      digest.scalar(initial_tag_at(region, offset));
  }
  return digest.value();
}

[[nodiscard]] std::uint64_t
observed_memory_digest(const Request &request,
                       const Observation &observation) noexcept {
  if (request.memory.empty())
    return instruction_evidence::empty_memory_digest();
  if (observation.status != ExecutionStatus::executed ||
      observation.memory.size() != request.memory.size())
    return 0;
  instruction_evidence::Digest digest;
  digest.scalar(request.memory.size());
  for (std::size_t index = 0; index < request.memory.size(); ++index) {
    const auto &region = request.memory[index];
    const auto &captured = observation.memory[index];
    if (!captured.captured || captured.address != region.address ||
        captured.bytes.size() != region.initial.size())
      return 0;
    digest.scalar(region.address);
    digest.scalar(captured.bytes.size());
    digest.boolean(region.readable);
    digest.boolean(region.writable);
    digest.boolean(region.executable);
    digest.boolean(region.guest_code);
    digest.boolean(region.guest_writable);
    digest.bytes(captured.bytes.data(), captured.bytes.size());
    for (std::size_t offset = 0; offset < captured.bytes.size(); offset += 16U)
      digest.scalar<std::uint8_t>(0);
  }
  return digest.value();
}

[[nodiscard]] std::string tsv_text(std::string_view text) {
  std::string result{text};
  for (auto &character : result)
    if (character == '\t' || character == '\r' || character == '\n')
      character = ' ';
  return result.empty() ? "-" : result;
}

[[nodiscard]] std::string stable_source_path(std::string_view path) {
  for (const auto marker : {std::string_view{"tests/"},
                            std::string_view{"darwin/"},
                            std::string_view{"scripts/"}}) {
    if (path.starts_with(marker))
      return std::string{path};
    const auto needle = std::string{"/"} + std::string{marker};
    const auto found = path.rfind(needle);
    if (found != std::string_view::npos)
      return std::string{path.substr(found + 1U)};
  }
  return std::filesystem::path{path}.filename().string();
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

struct Row final {
  std::string oracle_case_id;
  std::string ctest;
  std::string case_name;
  std::string source;
  std::uint_least32_t line{};
  std::uint32_t instruction{};
  std::uint32_t guest_page_size{};
  std::string required_features;
  std::string observed_features;
  Eligibility eligibility{};
  ExecutionStatus status{};
  Event event{};
  std::string observation_boundary;
  int signal{};
  std::uint64_t fault_address{};
  std::uint64_t initial_state_digest{};
  std::uint64_t initial_memory_digest{};
  std::uint64_t observed_state_digest{};
  std::uint64_t observed_memory_digest{};
  std::uint64_t timeout_ms{};

  [[nodiscard]] bool operator<(const Row &other) const noexcept {
    return std::tie(oracle_case_id, ctest, case_name, source, line, instruction,
                    guest_page_size, required_features, observed_features,
                    eligibility, status, event, observation_boundary, signal,
                    fault_address,
                    initial_state_digest, initial_memory_digest,
                    observed_state_digest, observed_memory_digest, timeout_ms) <
           std::tie(other.oracle_case_id, other.ctest, other.case_name,
                    other.source, other.line, other.instruction,
                    other.guest_page_size, other.required_features,
                    other.observed_features, other.eligibility, other.status,
                    other.event, other.observation_boundary, other.signal,
                    other.fault_address,
                    other.initial_state_digest, other.initial_memory_digest,
                    other.observed_state_digest, other.observed_memory_digest,
                    other.timeout_ms);
  }
};

class Recorder final {
public:
  ~Recorder() {
    if (rows_.empty())
      return;
    const auto *base = std::getenv("UML_INSTRUCTION_EVIDENCE");
    if (base == nullptr || base[0] == '\0')
      return;
    const auto path = std::string{base} + ".native";
    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::path{path}.parent_path(), error);
    if (error)
      std::abort();
    auto *output = std::fopen(path.c_str(), "w");
    if (output == nullptr)
      std::abort();
    std::fprintf(output,
                 "oracle_case_id\tctest\tcase\tsource\tline\tencoding\t"
                 "guest_page_size\trequired_features\tobserved_features\t"
                 "eligibility\texecution_status\tterminal_event\t"
                 "observation_boundary\tsignal\t"
                 "fault_address\tinitial_state_digest\t"
                 "initial_memory_digest\tobserved_state_digest\t"
                 "observed_memory_digest\ttimeout_ms\toccurrences\n");
    for (const auto &[row, occurrences] : rows_)
      std::fprintf(
          output,
          "%s\t%s\t%s\t%s\t%u\t%08x\t%u\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t"
          "%016llx\t%016llx\t%016llx\t%016llx\t%016llx\t%llu\t%llu\n",
          row.oracle_case_id.c_str(), row.ctest.c_str(), row.case_name.c_str(),
          row.source.c_str(), static_cast<unsigned>(row.line), row.instruction,
          row.guest_page_size, row.required_features.c_str(),
          row.observed_features.c_str(), name(row.eligibility).data(),
          name(row.status).data(), name(row.event).data(),
          row.observation_boundary.c_str(), row.signal,
          static_cast<unsigned long long>(row.fault_address),
          static_cast<unsigned long long>(row.initial_state_digest),
          static_cast<unsigned long long>(row.initial_memory_digest),
          static_cast<unsigned long long>(row.observed_state_digest),
          static_cast<unsigned long long>(row.observed_memory_digest),
          static_cast<unsigned long long>(row.timeout_ms),
          static_cast<unsigned long long>(occurrences));
    if (std::fclose(output) != 0)
      std::abort();
  }

  void add(std::string_view oracle_case_id, std::string_view case_name,
           std::uint32_t instruction,
           const ::uml::cpu::ArmFeatureSet &required,
           const ::uml::cpu::HostCapabilities &observed,
           const Request &request, const Observation &observation,
           std::uint64_t occurrences, const std::source_location &location) {
    const auto *ctest = std::getenv("UML_INSTRUCTION_EVIDENCE_TEST");
    const auto digests = observation_digests(request, observation);
    Row row{
        .oracle_case_id = tsv_text(oracle_case_id),
        .ctest = tsv_text(ctest == nullptr ? "-" : ctest),
        .case_name = tsv_text(case_name),
        .source = tsv_text(stable_source_path(location.file_name())),
        .line = location.line(),
        .instruction = instruction,
        .guest_page_size = DARWIN_UML_GUEST_PAGE_SIZE,
        .required_features = feature_names(required),
        .observed_features = feature_names(observed.supported),
        .eligibility = observation.eligibility,
        .status = observation.status,
        .event = observation.event,
        .observation_boundary =
            std::string{observation_boundary(observation)},
        .signal = observation.signal,
        .fault_address = observation.fault_address,
        .initial_state_digest = digests.initial_state,
        .initial_memory_digest = digests.initial_memory,
        .observed_state_digest = digests.observed_state,
        .observed_memory_digest = digests.observed_memory,
        .timeout_ms = static_cast<std::uint64_t>(request.timeout.count()),
    };
    rows_[std::move(row)] += occurrences;
  }

private:
  std::map<Row, std::uint64_t> rows_;
};

} // namespace

std::string_view
observation_boundary(const Observation &observation) noexcept {
  if (observation.status != ExecutionStatus::executed)
    return "none";
  switch (observation.event) {
  case Event::completed:
    return "single-instruction-pc+4";
  case Event::control_target:
    return "single-instruction-control-target";
  case Event::illegal_instruction:
  case Event::breakpoint:
  case Event::memory_fault:
  case Event::floating_point_trap:
    return "faulting-instruction-pc+0";
  case Event::none:
    return "none";
  }
  return "none";
}

ObservationDigests observation_digests(const Request &request,
                                       const Observation &observation) noexcept {
  return {
      .initial_state =
          instruction_evidence::architecture_state_digest(request.initial),
      .initial_memory = initial_memory_digest(request),
      .observed_state =
          instruction_evidence::architecture_state_digest(observation.state),
      .observed_memory = observed_memory_digest(request, observation),
  };
}

void record_observation(
    std::string_view oracle_case_id, std::string_view case_name,
    std::uint32_t instruction, const ::uml::cpu::ArmFeatureSet &required,
    const ::uml::cpu::HostCapabilities &observed, const Request &request,
    const Observation &observation, std::uint64_t occurrences,
    const std::source_location &location) {
  static Recorder recorder;
  recorder.add(oracle_case_id, case_name, instruction, required, observed,
               request, observation, occurrences, location);
}

} // namespace uml::emulator::tests::native_oracle
