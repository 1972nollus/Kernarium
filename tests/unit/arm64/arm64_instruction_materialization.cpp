// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_instruction_materialization.hpp"

#include "cpu/host_capabilities.hpp"
#include "instruction_evidence_digest.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace uml::emulator::tests {
namespace {

struct PayloadFingerprint final {
  std::uint64_t fnv{};
  std::size_t size{};

  [[nodiscard]] bool operator==(const PayloadFingerprint &) const noexcept =
      default;
};

[[noreturn]] void configuration_error(const std::string &message) {
  std::fprintf(stderr, "invalid instruction input materialization: %s\n",
               message.c_str());
  std::abort();
}

[[nodiscard]] std::string canonical_output_path(const char *path) {
  const std::filesystem::path requested{path};
  std::error_code error;
  const auto parent = std::filesystem::canonical(requested.parent_path(), error);
  if (error || requested.filename().empty())
    configuration_error("materialization output parent is not canonical");
  return (parent / requested.filename()).string();
}

[[nodiscard]] std::string lock_marker(std::string_view test,
                                      std::string_view evidence_path) {
  return "v1\n" + std::string{test} + "\n" + std::string{evidence_path};
}

[[nodiscard]] mode_t output_mode(const std::string &path) {
  struct stat status {};
  if (::lstat(path.c_str(), &status) == 0) {
    if (!S_ISREG(status.st_mode))
      configuration_error("materialization output is not regular");
    return status.st_mode & 07777;
  }
  if (errno != ENOENT)
    configuration_error("cannot inspect materialization output");
  return 0644;
}

[[nodiscard]] int open_staged_output(const std::string &path,
                                     std::string &temporary) {
  temporary = path + ".producer.tmp";
  if (::unlink(temporary.c_str()) != 0 && errno != ENOENT)
    configuration_error("cannot remove stale materialization staging file");
  auto flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  const auto descriptor = ::open(temporary.c_str(), flags, 0600);
  if (descriptor < 0)
    configuration_error("cannot create materialization staging file");
  if (::fchmod(descriptor, output_mode(path)) != 0) {
    ::close(descriptor);
    configuration_error("cannot set materialization staging mode");
  }
  return descriptor;
}

void sync_parent_directory(const std::string &path) {
  auto flags = O_RDONLY | O_CLOEXEC;
#ifdef O_DIRECTORY
  flags |= O_DIRECTORY;
#endif
  const auto parent = std::filesystem::path{path}.parent_path();
  const auto descriptor = ::open(parent.c_str(), flags);
  if (descriptor < 0 || ::fsync(descriptor) != 0) {
    if (descriptor >= 0)
      ::close(descriptor);
    configuration_error("cannot sync materialization output directory");
  }
  if (::close(descriptor) != 0)
    configuration_error("cannot close materialization output directory");
}

[[nodiscard]] std::string stable_source_path(std::string_view path) {
  const auto marker = std::string_view{"tests/"};
  if (path.starts_with(marker))
    return std::string{path};
  const auto needle = std::string{"/"} + std::string{marker};
  const auto found = path.rfind(needle);
  if (found != std::string_view::npos)
    return std::string{path.substr(found + 1U)};
  return std::filesystem::path{path}.filename().string();
}

void append_json_string(std::string &output, std::string_view value) {
  constexpr char digits[] = "0123456789abcdef";
  output.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\b':
      output += "\\b";
      break;
    case '\f':
      output += "\\f";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20U) {
        output += "\\u00";
        output.push_back(digits[character >> 4U]);
        output.push_back(digits[character & 15U]);
      } else {
        output.push_back(static_cast<char>(character));
      }
      break;
    }
  }
  output.push_back('"');
}

template <typename Value>
void append_fixed_hex(std::string &output, Value value) {
  constexpr char digits[] = "0123456789abcdef";
  std::array<char, sizeof(Value) * 2U> text{};
  using Unsigned = std::make_unsigned_t<Value>;
  const auto raw = static_cast<Unsigned>(value);
  for (std::size_t index = 0; index < text.size(); ++index) {
    const auto shift = (text.size() - index - 1U) * 4U;
    text[index] = digits[(raw >> shift) & 15U];
  }
  append_json_string(output, std::string_view{text.data(), text.size()});
}

void append_bytes_hex(std::string &output, std::span<const std::byte> bytes) {
  constexpr char digits[] = "0123456789abcdef";
  output.push_back('"');
  for (const auto byte : bytes) {
    const auto value = std::to_integer<std::uint8_t>(byte);
    output.push_back(digits[value >> 4U]);
    output.push_back(digits[value & 15U]);
  }
  output.push_back('"');
}

void append_bool(std::string &output, bool value) {
  output += value ? "true" : "false";
}

void append_feature_names(std::string &output,
                          const ArmFeatureSet &features) {
  output.push_back('[');
  bool first = true;
  for (std::size_t index = 0; index < ::uml::cpu::arm_feature_count; ++index) {
    if (!features.test(index))
      continue;
    if (!first)
      output.push_back(',');
    append_json_string(output, ::uml::cpu::arm_feature_tokens[index]);
    first = false;
  }
  output.push_back(']');
}

[[nodiscard]] std::string observed_feature_names(
    const ArmFeatureSet &required, const ArmFeatureSet &observed) {
  if (required.none())
    return "baseline=1";
  std::string result;
  for (std::size_t index = 0; index < ::uml::cpu::arm_feature_count; ++index) {
    if (!required.test(index))
      continue;
    if (!result.empty())
      result.push_back('+');
    result += ::uml::cpu::arm_feature_tokens[index];
    result += observed.test(index) ? "=1" : "=0";
  }
  return result;
}

[[nodiscard]] std::string missing_feature_names(
    const ArmFeatureSet &required, const ArmFeatureSet &observed) {
  std::string result;
  for (std::size_t index = 0; index < ::uml::cpu::arm_feature_count; ++index) {
    if (!required.test(index) || observed.test(index))
      continue;
    if (!result.empty())
      result.push_back('+');
    result += ::uml::cpu::arm_feature_tokens[index];
  }
  return result.empty() ? "none" : result;
}

[[nodiscard]] constexpr std::string_view
result_name(InstructionEngineResult result) noexcept {
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

[[nodiscard]] constexpr std::string_view
scope_name(InstructionNativeScope scope) noexcept {
  return scope == InstructionNativeScope::instruction ? "instruction"
                                                       : "program";
}

[[nodiscard]] std::string callback_identity(std::string_view kind,
                                            std::string_view source,
                                            std::uint_least32_t line) {
  return std::string{kind} + "@" + std::string{source} + ":" +
         std::to_string(line);
}

void append_callback_contract(std::string &output, bool present,
                              std::string_view identity,
                              std::string_view descriptor_key,
                              std::string_view descriptor_id) {
  if (!present) {
    output += "null";
    return;
  }
  output += "{\"identity\":";
  append_json_string(output, identity);
  output += ",\"";
  output += descriptor_key;
  output += "\":";
  append_json_string(output, descriptor_id);
  output.push_back('}');
}

void append_state(std::string &output, const arm64::CpuState &state) {
  output += "{\"x\":[";
  for (std::size_t index = 0; index < state.registers.size(); ++index) {
    if (index != 0)
      output.push_back(',');
    append_fixed_hex(output, state.registers[index]);
  }
  output += "],\"sp\":";
  append_fixed_hex(output, state.sp);
  output += ",\"pc\":";
  append_fixed_hex(output, state.pc);
  output += ",\"nzcv\":";
  append_fixed_hex(output, state.nzcv);
  output += ",\"vectors\":[";
  for (std::size_t index = 0; index < state.vectors.size(); ++index) {
    if (index != 0)
      output.push_back(',');
    append_bytes_hex(output, state.vectors[index].bytes);
  }
  output += "],\"fpcr\":";
  append_fixed_hex(output, state.fpcr);
  output += ",\"fpsr\":";
  append_fixed_hex(output, state.fpsr);
  output += ",\"fpmr\":";
  append_fixed_hex(output, state.fpmr);
  output += ",\"tls\":";
  append_fixed_hex(output, state.tls);
  output += ",\"mte\":{\"enabled\":";
  append_bool(output, state.mte_enabled);
  output += ",\"exclude\":";
  append_fixed_hex(output, state.mte_exclude);
  output += ",\"tag_choices\":";
  append_bytes_hex(output,
                   std::as_bytes(std::span{state.mte_tag_choices}));
  output += ",\"random_tags\":";
  append_bool(output, state.mte_random_tags);
  output += ",\"seed\":";
  append_fixed_hex(output, state.mte_seed);
  output += ",\"tag\":";
  append_fixed_hex(output, state.mte_tag);
  output += ",\"random_state\":";
  append_fixed_hex(output, state.mte_random_state);
  output += "},\"checked_arithmetic\":{\"add_enabled\":";
  append_bool(output, state.cpa_enabled);
  output += ",\"multiply_enabled\":";
  append_bool(output, state.cpa_multiply_enabled);
  output += ",\"accdata\":";
  append_fixed_hex(output, state.accdata);
  output += "},\"pstate\":{\"ssbs\":";
  append_bool(output, state.pstate_ssbs);
  output += ",\"dit\":";
  append_bool(output, state.pstate_dit);
  output += ",\"tco\":";
  append_bool(output, state.pstate_tco);
  output += ",\"sm\":";
  append_bool(output, state.pstate_sm);
  output += ",\"za\":";
  append_bool(output, state.pstate_za);
  output += ",\"daif_access\":";
  append_bool(output, state.pstate_daif_access);
  output += ",\"daif\":";
  append_fixed_hex(output, state.pstate_daif);
  output += "},\"exclusive_monitor\":{\"address\":";
  append_fixed_hex(output, state.exclusive_address);
  output += ",\"key\":";
  append_fixed_hex(output, static_cast<std::uint64_t>(state.exclusive_key));
  output += ",\"value\":";
  append_fixed_hex(output, state.exclusive_value);
  output += ",\"value2\":";
  append_fixed_hex(output, state.exclusive_value2);
  output += ",\"version\":";
  append_fixed_hex(output, state.exclusive_version);
  output += ",\"size\":";
  append_fixed_hex(output, state.exclusive_size);
  output += "},\"guarded_stack\":{\"pointer\":";
  append_fixed_hex(output, state.gcs_pointer);
  output += ",\"implemented\":";
  append_bool(output, state.gcs_implemented);
  output += ",\"push_enabled\":";
  append_bool(output, state.gcs_push_enabled);
  output += ",\"store_enabled\":";
  append_bool(output, state.gcs_store_enabled);
  output += "},\"signing_keys\":[";
  for (std::size_t index = 0; index < state.pointer_auth_keys.size(); ++index) {
    if (index != 0)
      output.push_back(',');
    output += "{\"low\":";
    append_fixed_hex(output, state.pointer_auth_keys[index].low);
    output += ",\"high\":";
    append_fixed_hex(output, state.pointer_auth_keys[index].high);
    output.push_back('}');
  }
  output += "],\"signing_enabled\":";
  append_bool(output, state.pointer_authentication_enabled);
  output.push_back('}');
}

[[nodiscard]] std::uint8_t tag_at(std::span<const AllocationTag> tags,
                                  std::size_t offset) noexcept {
  std::uint8_t result{};
  for (const auto tag : tags)
    if (tag.offset == offset)
      result = tag.value;
  return result;
}

void append_memory(std::string &output,
                   std::span<const MemoryRegion> regions) {
  output.push_back('[');
  for (std::size_t index = 0; index < regions.size(); ++index) {
    const auto &region = regions[index];
    if (index != 0)
      output.push_back(',');
    output += "{\"address\":";
    append_fixed_hex(output, region.address);
    output += ",\"size\":";
    append_fixed_hex(output, static_cast<std::uint64_t>(region.initial.size()));
    output += ",\"initial_bytes\":";
    append_bytes_hex(output, region.initial);
    output += ",\"expected_bytes\":";
    append_bytes_hex(output, region.expected.empty() ? region.initial
                                                      : region.expected);
    std::vector<std::byte> initial_tags;
    std::vector<std::byte> expected_tags;
    for (std::size_t offset = 0; offset < region.initial.size(); offset += 16U) {
      const auto initial = tag_at(region.initial_tags, offset);
      auto expected = initial;
      for (const auto tag : region.expected_tags)
        if (tag.offset == offset)
          expected = tag.value;
      initial_tags.push_back(static_cast<std::byte>(initial));
      expected_tags.push_back(static_cast<std::byte>(expected));
    }
    output += ",\"initial_tags\":";
    append_bytes_hex(output, initial_tags);
    output += ",\"expected_tags\":";
    append_bytes_hex(output, expected_tags);
    output += ",\"permissions\":{\"readable\":";
    append_bool(output, region.readable);
    output += ",\"writable\":";
    append_bool(output, region.writable);
    output += ",\"executable\":";
    append_bool(output, region.executable);
    output += ",\"guest_code\":";
    append_bool(output, region.guest_code);
    output += ",\"guest_writable\":";
    append_bool(output, region.guest_writable);
    output += "}}";
  }
  output.push_back(']');
}

[[nodiscard]] PayloadFingerprint
payload_fingerprint(std::string_view payload) noexcept {
  instruction_evidence::Digest digest;
  digest.bytes(payload.data(), payload.size());
  return {.fnv = digest.value(), .size = payload.size()};
}

[[nodiscard]] std::string content_id(std::string_view prefix,
                                     std::string_view payload) {
  return std::string{prefix} + instruction_evidence::sha256_hex(payload);
}

[[nodiscard]] std::string render_program_payload(
    const InstructionInputMaterialization &materialization) {
  if (materialization.installed_code_page.size() !=
      DARWIN_UML_GUEST_PAGE_SIZE)
    configuration_error("installed code page has the wrong size");
  std::vector<ProgramWord> words{materialization.code.begin(),
                                 materialization.code.end()};
  std::sort(words.begin(), words.end(), [](const auto &left, const auto &right) {
    return left.offset < right.offset;
  });
  if (words.empty())
    configuration_error("concrete input has no program words");
  std::vector<std::byte> reconstructed(DARWIN_UML_GUEST_PAGE_SIZE);
  std::set<std::size_t> offsets;
  bool has_entry = false;
  for (const auto &word : words) {
    if (word.offset % 4U != 0U ||
        word.offset > reconstructed.size() - sizeof(word.instruction) ||
        !offsets.insert(word.offset).second)
      configuration_error("program contains an invalid/duplicate word offset");
    has_entry = has_entry || word.offset == materialization.entry_offset;
    for (std::size_t byte = 0; byte < sizeof(word.instruction); ++byte)
      reconstructed[word.offset + byte] =
          static_cast<std::byte>(word.instruction >> (byte * 8U));
  }
  if (!has_entry)
    configuration_error("program entry does not name a declared word");
  if (!std::equal(reconstructed.begin(), reconstructed.end(),
                  materialization.installed_code_page.begin()))
    configuration_error(
        "installed code page differs from zero-fill sparse reconstruction");

  std::string output;
  output.reserve(512U + words.size() * 64U);
  output += "{\"guest_page_size\":";
  append_fixed_hex(output,
                   static_cast<std::uint32_t>(DARWIN_UML_GUEST_PAGE_SIZE));
  output += ",\"code_base\":";
  append_fixed_hex(output, engine_test_code_base);
  output += ",\"entry_offset\":";
  append_fixed_hex(output,
                   static_cast<std::uint64_t>(materialization.entry_offset));
  output += ",\"page_encoding\":\"zero-fill-sparse-words/v1\",\"words\":[";
  for (std::size_t index = 0; index < words.size(); ++index) {
    if (index != 0)
      output.push_back(',');
    output += "{\"offset\":";
    append_fixed_hex(output, static_cast<std::uint64_t>(words[index].offset));
    output += ",\"word\":";
    append_fixed_hex(output, words[index].instruction);
    output.push_back('}');
  }
  output += "],\"reconstructed_page_sha256\":";
  append_json_string(
      output, instruction_evidence::sha256_hex(std::span{reconstructed}));
  output += ",\"permissions\":{\"readable\":true,\"writable\":false,"
            "\"executable\":true,\"guest_code\":true,"
            "\"guest_writable\":false}}";
  return output;
}

[[nodiscard]] std::string render_state_payload(const arm64::CpuState &state) {
  std::string output;
  output.reserve(4096U);
  append_state(output, state);
  return output;
}

[[nodiscard]] std::string render_memory_payload(
    std::span<const MemoryRegion> memory) {
  std::string output;
  append_memory(output, memory);
  return output;
}

[[nodiscard]] std::string render_contract_payload(
    const InstructionInputMaterialization &materialization,
    std::string_view ctest, std::string_view source,
    std::uint_least32_t line, std::uint32_t evidence_word,
    std::string_view program_id, std::string_view initial_state_id,
    std::string_view expected_state_id, std::string_view memory_id) {
  std::string output;
  output.reserve(4096U);
  output += "{\"identity\":{\"ctest\":";
  append_json_string(output, ctest);
  output += ",\"case\":";
  append_json_string(output, materialization.case_name);
  output += ",\"source\":";
  append_json_string(output, source);
  output += ",\"line\":";
  append_fixed_hex(output, line);
  output += ",\"scope\":";
  append_json_string(output, scope_name(materialization.native_scope));
  output += ",\"evidence_word\":";
  append_fixed_hex(output, evidence_word);
  output += "},\"program_id\":";
  append_json_string(output, program_id);
  output += ",\"initial_state_id\":";
  append_json_string(output, initial_state_id);
  output += ",\"expected_state_id\":";
  append_json_string(output, expected_state_id);
  output += ",\"memory_id\":";
  append_json_string(output, memory_id);
  output += ",\"feature_profile\":{\"native_required\":";
  append_feature_names(output, materialization.native.features());
  output += ",\"native_observed\":";
  append_json_string(
      output,
      observed_feature_names(materialization.native_observation.required,
                             materialization.native_observation.observed));
  output += ",\"native_missing\":";
  append_json_string(
      output,
      missing_feature_names(materialization.native_observation.required,
                            materialization.native_observation.observed));
  output += "},\"terminal\":{\"event\":";
  append_json_string(output, materialization.expected_event);
  output += ",\"retirement\":";
  append_json_string(output, materialization.expected_retirement);
  output += ",\"instruction_budget\":";
  append_fixed_hex(output,
                   static_cast<std::uint64_t>(materialization.instruction_budget));
  output += ",\"state_oracle\":";
  append_json_string(output, materialization.expected_state_oracle);
  output += ",\"fault\":";
  append_json_string(output, materialization.expected_fault);
  output += ",\"syndrome\":";
  append_fixed_hex(output, materialization.expected_syndrome);
  output += ",\"invalid_encoding\":";
  append_bool(output, materialization.invalid_encoding);
  output += "},\"engine_results\":{\"interpreter\":";
  append_json_string(output, result_name(materialization.engines.interpreter));
  output += ",\"gadgets\":";
  append_json_string(output, result_name(materialization.engines.gadgets));
  output += ",\"jit\":";
  append_json_string(output, result_name(materialization.engines.jit));
  output += "},\"representability\":{\"status\":";
  append_json_string(output, "pending-official-model-projection");
  output += ",\"obligations\":[],\"callback_contracts\":{\"validator\":";
  append_callback_contract(
      output, materialization.has_custom_validator,
      callback_identity("custom-validator", source, line), "relation_id",
      materialization.validator_relation_id);
  output += ",\"before_engine\":";
  append_callback_contract(
      output, materialization.has_before_engine_hook,
      callback_identity("before-engine-hook", source, line), "effect_id",
      materialization.before_engine_effect_id);
  output += ",\"after_engine\":";
  append_callback_contract(
      output, materialization.has_after_engine_hook,
      callback_identity("after-engine-hook", source, line), "effect_id",
      materialization.after_engine_effect_id);
  output += "},\"excluded_engine_private_fields\":["
            "\"deferred_fp_representation\","
            "\"gadget_host_environment\","
            "\"gadget_indirect_miss\",\"gadget_zero\"]},"
            "\"integrity\":{\"initial_state_digest\":";
  append_fixed_hex(output, materialization.initial_state_digest);
  output += ",\"initial_memory_digest\":";
  append_fixed_hex(output, materialization.initial_memory_digest);
  output += ",\"expected_state_digest\":";
  append_fixed_hex(output, materialization.expected_state_digest);
  output += ",\"expected_memory_digest\":";
  append_fixed_hex(output, materialization.expected_memory_digest);
  output += "}}";
  return output;
}

class Recorder final {
public:
  ~Recorder() {
    if (output_ == nullptr)
      return;
    if (gzclose(output_) != Z_OK) {
      output_ = nullptr;
      configuration_error("cannot close " + output_path_);
    }
    output_ = nullptr;
    if (::fsync(sync_descriptor_) != 0 || ::close(sync_descriptor_) != 0)
      configuration_error("cannot sync " + temporary_path_);
    sync_descriptor_ = -1;
    if (::rename(temporary_path_.c_str(), output_path_.c_str()) != 0)
      configuration_error("cannot publish " + output_path_);
    sync_parent_directory(output_path_);
  }

  void add(const InstructionInputMaterialization &materialization,
           const std::source_location &location) {
    configure();
    if (!enabled_)
      return;
    if (materialization.native.features() !=
        materialization.native_observation.required)
      configuration_error(
          "native requirement differs from the engine evidence contract");
    if (materialization.evidence_words.empty())
      configuration_error("concrete input has no evidence word");
    if (materialization.instruction_budget == 0U)
      configuration_error("concrete input has zero instruction budget");
    const auto source = stable_source_path(location.file_name());
    const auto require_descriptor = [](bool present, std::string_view value,
                                       std::string_view kind) {
      if (present != !value.empty())
        configuration_error(std::string{kind} +
                            " callback/descriptor presence differs");
    };
    require_descriptor(materialization.has_custom_validator,
                       materialization.validator_relation_id, "validator");
    require_descriptor(materialization.has_before_engine_hook,
                       materialization.before_engine_effect_id,
                       "before-engine");
    require_descriptor(materialization.has_after_engine_hook,
                       materialization.after_engine_effect_id,
                       "after-engine");
    const auto expected_state_oracle =
        materialization.has_custom_validator
            ? callback_identity("custom-validator", source, location.line())
            : std::string{"exact"};
    if (materialization.expected_state_oracle != expected_state_oracle)
      configuration_error("terminal state-oracle identity differs from callback");

    const auto program_payload = render_program_payload(materialization);
    const auto initial_payload = render_state_payload(materialization.initial);
    const auto expected_payload = render_state_payload(materialization.expected);
    const auto memory_payload = render_memory_payload(materialization.memory);
    const auto program_id = emit_blob(
        "uml.a64.program-blob/v2", "program-", program_payload);
    const auto initial_state_id = emit_blob(
        "uml.a64.state-blob/v2", "state-", initial_payload);
    const auto expected_state_id = emit_blob(
        "uml.a64.state-blob/v2", "state-", expected_payload);
    const auto memory_id = emit_blob(
        "uml.a64.memory-blob/v2", "memory-", memory_payload);
    for (const auto evidence_word : materialization.evidence_words) {
      if (std::none_of(materialization.code.begin(), materialization.code.end(),
                       [evidence_word](const auto &word) {
                         return word.instruction == evidence_word;
                       }))
        configuration_error("evidence word is absent from sparse program");
      const auto payload = render_contract_payload(
          materialization, test_name_, source, location.line(), evidence_word,
          program_id, initial_state_id, expected_state_id, memory_id);
      const auto contract_id = content_id("input-contract-", payload);
      const auto fingerprint = payload_fingerprint(payload);
      const auto found = contracts_.find(contract_id);
      if (found != contracts_.end() && !(found->second == fingerprint))
        configuration_error(
            "one exact contract ID has conflicting payloads: id=" +
            contract_id + " existing-size=" +
            std::to_string(found->second.size) + " new-size=" +
            std::to_string(fingerprint.size));
      if (found == contracts_.end())
        contracts_.emplace(contract_id, fingerprint);
      emit_contract(contract_id, payload);
    }
  }

private:
  [[nodiscard]] std::string emit_blob(std::string_view schema,
                                      std::string_view prefix,
                                      std::string_view payload) {
    const auto id = content_id(prefix, payload);
    const auto fingerprint = payload_fingerprint(payload);
    const auto found = blobs_.find(id);
    if (found != blobs_.end()) {
      if (!(found->second == fingerprint))
        configuration_error("one content ID has conflicting blob payloads");
      return id;
    }
    blobs_.emplace(id, fingerprint);
    std::string line{"{\"schema\":"};
    append_json_string(line, schema);
    line += ",\"id\":";
    append_json_string(line, id);
    line += ",\"payload\":";
    line += payload;
    line.push_back('}');
    write_line(line);
    return id;
  }

  void emit_contract(std::string_view contract_id, std::string_view payload) {
    std::string line{"{\"schema\":\"uml.a64.input-contract/v4\",\"id\":"};
    append_json_string(line, contract_id);
    line += ",\"payload\":";
    line += payload;
    line += ",\"raw_occurrences\":\"0000000000000001\"}";
    write_line(line);
  }

  void write_line(std::string_view line) {
    const auto write = [this](const char *data, std::size_t size) {
      while (size != 0U) {
        const auto chunk = static_cast<unsigned int>(std::min<std::size_t>(
            size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        if (gzwrite(output_, data, chunk) != static_cast<int>(chunk))
          configuration_error("cannot write " + output_path_);
        data += chunk;
        size -= chunk;
      }
    };
    write(line.data(), line.size());
    write("\n", 1U);
  }

  void configure() {
    if (configured_)
      return;
    configured_ = true;
    const auto *explicit_output =
        std::getenv("UML_INSTRUCTION_INPUT_MATERIALIZATION");
    const bool explicit_enabled =
        explicit_output != nullptr && explicit_output[0] != '\0' &&
        explicit_output[0] != '0';
    if (!explicit_enabled)
      return;
    const auto *test = std::getenv("UML_INSTRUCTION_EVIDENCE_TEST");
    if (test == nullptr || test[0] == '\0')
      configuration_error("evidence test name is unset");
    const auto *base = std::getenv("UML_INSTRUCTION_EVIDENCE");
    if (base == nullptr || base[0] == '\0')
      configuration_error("evidence output is unset");
    const auto canonical_base = canonical_output_path(base);
    const auto *locked =
        std::getenv("UML_INSTRUCTION_EVIDENCE_LOCKED");
    if (locked == nullptr ||
        std::string_view{locked} != lock_marker(test, canonical_base))
      configuration_error("evidence producer runner lock binding differs");
    const auto expected_output = canonical_base + ".inputs.jsonl.gz";
    output_path_ = canonical_output_path(explicit_output);
    if (output_path_ != expected_output)
      configuration_error(
          "materialization output differs from evidence producer binding");
    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::path{output_path_}.parent_path(), error);
    if (error)
      configuration_error("cannot create output directory: " +
                          error.message());
    const auto descriptor = open_staged_output(output_path_, temporary_path_);
#ifdef F_DUPFD_CLOEXEC
    sync_descriptor_ = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
#else
    sync_descriptor_ = ::dup(descriptor);
    if (sync_descriptor_ >= 0)
      ::fcntl(sync_descriptor_, F_SETFD, FD_CLOEXEC);
#endif
    if (sync_descriptor_ < 0) {
      ::close(descriptor);
      configuration_error("cannot retain materialization staging descriptor");
    }
    output_ = gzdopen(descriptor, "wb6");
    if (output_ == nullptr) {
      ::close(sync_descriptor_);
      sync_descriptor_ = -1;
      ::close(descriptor);
      configuration_error("cannot write " + temporary_path_);
    }
    if (gzbuffer(output_, 1U << 20U) != 0)
      configuration_error("cannot configure compression for " +
                          temporary_path_);
    test_name_ = test;
    enabled_ = true;
  }

  bool configured_{};
  bool enabled_{};
  std::string test_name_;
  std::string output_path_;
  std::string temporary_path_;
  int sync_descriptor_{-1};
  gzFile output_{};
  std::map<std::string, PayloadFingerprint> blobs_;
  std::map<std::string, PayloadFingerprint> contracts_;
};

} // namespace

void record_instruction_input_materialization(
    const InstructionInputMaterialization &materialization,
    const std::source_location &location) {
  static Recorder recorder;
  recorder.add(materialization, location);
}

} // namespace uml::emulator::tests
