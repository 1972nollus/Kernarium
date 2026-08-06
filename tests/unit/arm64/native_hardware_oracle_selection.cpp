// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_hardware_oracle_selection.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace uml::emulator::tests::native_oracle {
namespace {

constexpr std::size_t official_allocation_count = 2396;

[[noreturn]] void configuration_error(std::string_view message) {
  std::fprintf(stderr, "invalid native evidence configuration: %.*s\n",
               static_cast<int>(message.size()), message.data());
  std::abort();
}

[[nodiscard]] std::vector<std::string> split_tsv(const std::string &line) {
  std::vector<std::string> fields;
  std::size_t begin = 0;
  for (;;) {
    const auto tab = line.find('\t', begin);
    fields.emplace_back(line.substr(begin, tab - begin));
    if (tab == std::string::npos)
      return fields;
    begin = tab + 1U;
  }
}

using ColumnMap = std::map<std::string, std::size_t, std::less<>>;

[[nodiscard]] ColumnMap columns_for(const std::string &header) {
  ColumnMap result;
  const auto fields = split_tsv(header);
  for (std::size_t index = 0; index < fields.size(); ++index)
    if (!result.emplace(fields[index], index).second)
      configuration_error("duplicate TSV column");
  return result;
}

[[nodiscard]] std::string_view field(
    const std::vector<std::string> &fields, const ColumnMap &columns,
    std::string_view name) {
  const auto iterator = columns.find(name);
  if (iterator == columns.end() || iterator->second >= fields.size())
    configuration_error("missing TSV column or field");
  return fields[iterator->second];
}

template <typename Value>
[[nodiscard]] Value parse_unsigned(std::string_view text, int base,
                                   std::string_view description) {
  Value result{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), result, base);
  if (error != std::errc{} || end != text.data() + text.size())
    configuration_error(description);
  return result;
}

struct BitConstraint final {
  std::uint32_t mask{};
  std::uint32_t value{};
  bool not_equal{};
};

[[nodiscard]] std::optional<std::vector<BitConstraint>>
parse_constraints(std::string_view text) {
  std::vector<BitConstraint> result;
  if (text == "none" || text.empty())
    return result;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const auto end = text.find(';', begin);
    const auto item = text.substr(begin, end - begin);
    const auto at = item.find('@');
    const auto colon = item.find(':', at == std::string_view::npos ? 0 : at);
    const auto not_equal = item.find("!=", colon == std::string_view::npos
                                               ? 0
                                               : colon);
    const auto equal = not_equal == std::string_view::npos
                           ? item.find('=', colon == std::string_view::npos
                                                ? 0
                                                : colon)
                           : not_equal;
    if (at == std::string_view::npos || colon == std::string_view::npos ||
        equal == std::string_view::npos)
      return std::nullopt;
    const auto offset_text = item.substr(at + 1U, colon - at - 1U);
    const auto width_end = not_equal == std::string_view::npos ? equal
                                                               : not_equal;
    const auto width_text = item.substr(colon + 1U, width_end - colon - 1U);
    const auto pattern = item.substr(
        equal + (not_equal == std::string_view::npos ? 1U : 2U));
    unsigned offset{};
    unsigned width{};
    const auto [offset_end, offset_error] = std::from_chars(
        offset_text.data(), offset_text.data() + offset_text.size(), offset);
    const auto [width_result_end, width_error] = std::from_chars(
        width_text.data(), width_text.data() + width_text.size(), width);
    if (offset_error != std::errc{} ||
        offset_end != offset_text.data() + offset_text.size() ||
        width_error != std::errc{} ||
        width_result_end != width_text.data() + width_text.size() ||
        width == 0 || width > 32U || offset > 31U ||
        width > 32U - offset || pattern.size() != width)
      return std::nullopt;
    BitConstraint constraint{.not_equal =
                                 not_equal != std::string_view::npos};
    for (unsigned index = 0; index < width; ++index) {
      const auto bit = offset + width - index - 1U;
      if (pattern[index] == 'x')
        continue;
      if (pattern[index] != '0' && pattern[index] != '1')
        return std::nullopt;
      constraint.mask |= std::uint32_t{1} << bit;
      if (pattern[index] == '1')
        constraint.value |= std::uint32_t{1} << bit;
    }
    result.push_back(constraint);
    if (end == std::string_view::npos)
      break;
    begin = end + 1U;
  }
  return result;
}

[[nodiscard]] bool constraints_match(
    std::uint32_t instruction,
    std::span<const BitConstraint> constraints) noexcept {
  for (const auto constraint : constraints) {
    const bool equal =
        (instruction & constraint.mask) == constraint.value;
    if (equal == constraint.not_equal)
      return false;
  }
  return true;
}

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

[[nodiscard]] std::array<std::byte, 32>
identity_digest(std::span<const std::byte> input) {
  std::vector<std::byte> message{input.begin(), input.end()};
  const auto bit_size = static_cast<std::uint64_t>(message.size()) * 8U;
  message.push_back(std::byte{0x80});
  while (message.size() % 64U != 56U)
    message.push_back(std::byte{});
  for (unsigned shift = 56U;; shift -= 8U) {
    message.push_back(static_cast<std::byte>(bit_size >> shift));
    if (shift == 0)
      break;
  }

  std::array<std::uint32_t, 8> state{
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  for (std::size_t block = 0; block < message.size(); block += 64U) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16U; ++index) {
      const auto offset = block + index * 4U;
      for (std::size_t byte = 0; byte < 4U; ++byte)
        words[index] = (words[index] << 8U) |
            std::to_integer<std::uint8_t>(message[offset + byte]);
    }
    for (std::size_t index = 16U; index < words.size(); ++index) {
      const auto first = std::rotr(words[index - 15U], 7) ^
          std::rotr(words[index - 15U], 18) ^
          (words[index - 15U] >> 3U);
      const auto second = std::rotr(words[index - 2U], 17) ^
          std::rotr(words[index - 2U], 19) ^
          (words[index - 2U] >> 10U);
      words[index] = words[index - 16U] + first + words[index - 7U] + second;
    }
    auto working = state;
    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto choice =
          (working[4] & working[5]) ^ (~working[4] & working[6]);
      const auto majority = (working[0] & working[1]) ^
          (working[0] & working[2]) ^ (working[1] & working[2]);
      const auto upper = std::rotr(working[0], 2) ^
          std::rotr(working[0], 13) ^ std::rotr(working[0], 22);
      const auto lower = std::rotr(working[4], 6) ^
          std::rotr(working[4], 11) ^ std::rotr(working[4], 25);
      const auto first = working[7] + lower + choice +
          round_constants[index] + words[index];
      const auto second = upper + majority;
      for (std::size_t position = 7; position > 0; --position)
        working[position] = working[position - 1U];
      working[4] += first;
      working[0] = first + second;
    }
    for (std::size_t index = 0; index < state.size(); ++index)
      state[index] += working[index];
  }
  std::array<std::byte, 32> result{};
  for (std::size_t index = 0; index < state.size(); ++index)
    for (std::size_t byte = 0; byte < 4U; ++byte)
      result[index * 4U + byte] = static_cast<std::byte>(
          state[index] >> ((3U - byte) * 8U));
  return result;
}

[[nodiscard]] std::string identity_text(const AllocationIdentity &identity) {
  const std::array fields{
      identity.stable_id,
      identity.architecture_file,
      identity.architecture_encoding,
      identity.architecture_mask,
      identity.architecture_value,
      identity.architecture_constraints,
      identity.decode_class,
      identity.decode_encoding,
      identity.decode_mask,
      identity.decode_value,
      identity.decode_constraints,
  };
  std::string result;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0)
      result.push_back('\0');
    result.append(fields[index]);
  }
  return result;
}

[[nodiscard]] std::string group_key(
    std::string_view stable_id, std::string_view architecture_file,
    std::string_view architecture_encoding,
    std::string_view architecture_mask, std::string_view architecture_value,
    std::string_view architecture_constraints,
    std::string_view allocation_kind, std::string_view decode_class,
    std::string_view decode_encoding, std::string_view decode_mask,
    std::string_view decode_value, std::string_view decode_constraints) {
  const std::array fields{stable_id, architecture_file, architecture_encoding,
                          architecture_mask, architecture_value,
                          architecture_constraints, allocation_kind,
                          decode_class, decode_encoding, decode_mask,
                          decode_value, decode_constraints};
  std::string result;
  for (const auto value : fields) {
    result.append(value);
    result.push_back('\0');
  }
  return result;
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

struct SourceChoice final {
  std::string producer;
  std::string source;
  std::uint_least32_t line{};
  SelectionScope scope{SelectionScope::program};
  bool exact_state{};
  bool ordinary_completion{};
  std::string aggregate_id;
};

struct DecodePartition final {
  std::string decode_class;
  std::string decode_encoding;
  std::string decode_mask;
  std::string decode_value;
  std::string decode_constraints;
};

[[nodiscard]] bool better_choice(const SourceChoice &candidate,
                                 const SourceChoice &current) noexcept {
  const auto candidate_rank = std::tuple{
      candidate.scope == SelectionScope::instruction ? 0 : 1,
      candidate.exact_state ? 0 : 1,
      candidate.ordinary_completion ? 0 : 1,
      candidate.producer, candidate.source, candidate.line,
      candidate.aggregate_id};
  const auto current_rank = std::tuple{
      current.scope == SelectionScope::instruction ? 0 : 1,
      current.exact_state ? 0 : 1,
      current.ordinary_completion ? 0 : 1,
      current.producer, current.source, current.line, current.aggregate_id};
  return candidate_rank < current_rank;
}

struct RequiredAllocation final {
  std::string status_id;
  std::string group;
  std::uint32_t architecture_mask{};
  std::uint32_t architecture_value{};
  std::vector<BitConstraint> architecture_constraints;
  std::uint32_t decode_mask{};
  std::uint32_t decode_value{};
  std::vector<BitConstraint> decode_constraints;
};

struct SelectedAllocation final {
  std::string status_id;
  std::string source;
  std::uint_least32_t line{};
  SelectionScope scope{SelectionScope::instruction};
  std::uint32_t architecture_mask{};
  std::uint32_t architecture_value{};
  std::vector<BitConstraint> architecture_constraints;
  std::uint32_t decode_mask{};
  std::uint32_t decode_value{};
  std::vector<BitConstraint> decode_constraints;
  bool observed{};
};

struct MaterializationCandidate final {
  AllocationMaterialization record;
  std::uint32_t architecture_mask{};
  std::uint32_t architecture_value{};
  std::vector<BitConstraint> architecture_constraints;
  std::uint32_t decode_mask{};
  std::uint32_t decode_value{};
  std::vector<BitConstraint> decode_constraints;
};

[[nodiscard]] bool candidate_matches(
    const std::uint32_t instruction, const SelectionScope scope,
    const std::string_view source, const SelectedAllocation &candidate) {
  return !candidate.observed && candidate.scope == scope &&
      candidate.source == source &&
      (instruction & candidate.architecture_mask) ==
          candidate.architecture_value &&
      (instruction & candidate.decode_mask) == candidate.decode_value &&
      constraints_match(instruction, candidate.architecture_constraints) &&
      constraints_match(instruction, candidate.decode_constraints);
}

[[nodiscard]] bool exact_lower_hex(const std::string_view value,
                                   const std::size_t digits) noexcept {
  return value.size() == digits &&
      std::ranges::all_of(value, [](const char character) {
        return (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f');
      });
}

[[nodiscard]] bool exact_prefixed_hex(const std::string_view value,
                                      const std::string_view prefix,
                                      const std::size_t digits) noexcept {
  return value.starts_with(prefix) &&
      exact_lower_hex(value.substr(prefix.size()), digits);
}

[[nodiscard]] bool candidate_matches(
    const std::uint32_t instruction, const SelectionScope scope,
    const std::string_view source,
    const MaterializationCandidate &candidate) {
  return candidate.record.scope == scope &&
      candidate.record.source_file == source &&
      (instruction & candidate.architecture_mask) ==
          candidate.architecture_value &&
      (instruction & candidate.decode_mask) == candidate.decode_value &&
      constraints_match(instruction, candidate.architecture_constraints) &&
      constraints_match(instruction, candidate.decode_constraints);
}

class SelectionRegistry final {
public:
  SelectionRegistry() { configure(); }

  ~SelectionRegistry() {
    if (!enabled_)
      return;
    std::size_t missing = 0;
    for (const auto &allocation : allocations_)
      if (!allocation.observed) {
        if (missing < 10U)
          std::fprintf(stderr,
                       "native evidence allocation was not observed: %s at "
                       "%s:%u\n",
                       allocation.status_id.c_str(), allocation.source.c_str(),
                       static_cast<unsigned>(allocation.line));
        ++missing;
      }
    if (missing != 0) {
      std::fprintf(stderr,
                   "native evidence missed %zu reviewed allocations for %s\n",
                   missing, test_name_.c_str());
      std::abort();
    }
  }

  [[nodiscard]] AllocationSelection take(
      std::uint32_t instruction, SelectionScope scope,
      const std::source_location &location) {
    AllocationSelection result;
    if (!enabled_)
      return result;
    const auto range = lines_.equal_range(location.line());
    const auto source = stable_source_path(location.file_name());
    for (auto iterator = range.first; iterator != range.second; ++iterator) {
      auto &allocation = allocations_[iterator->second];
      if (!candidate_matches(instruction, scope, source, allocation))
        continue;
      allocation.observed = true;
      result.allocation_status_ids.push_back(allocation.status_id);
    }
    if (!result.allocation_status_ids.empty())
      return result;

    /* Source lines are evidence coordinates, not allocation identity. Keep a
     * narrow compatibility window for a call that moved inside the same
     * helper after the reviewed aggregate was produced. The source path,
     * scope, and complete architecture/decode allocation still have to match,
     * and only the unique nearest reviewed line is admitted. */
    constexpr std::uint_least32_t maximum_source_line_drift = 8U;
    std::optional<std::uint_least32_t> nearest_distance;
    std::optional<std::uint_least32_t> nearest_line;
    std::vector<std::size_t> nearest;
    bool ambiguous_nearest_line = false;
    for (std::size_t index = 0; index < allocations_.size(); ++index) {
      const auto &allocation = allocations_[index];
      if (!candidate_matches(instruction, scope, source, allocation))
        continue;
      const auto distance = allocation.line > location.line()
          ? allocation.line - location.line()
          : location.line() - allocation.line;
      if (distance > maximum_source_line_drift ||
          (nearest_distance && distance > *nearest_distance))
        continue;
      if (!nearest_distance || distance < *nearest_distance) {
        nearest_distance = distance;
        nearest_line = allocation.line;
        nearest.clear();
        ambiguous_nearest_line = false;
      } else if (allocation.line != *nearest_line) {
        ambiguous_nearest_line = true;
      }
      if (!ambiguous_nearest_line && allocation.line == *nearest_line)
        nearest.push_back(index);
    }
    if (ambiguous_nearest_line)
      return result;
    for (const auto index : nearest) {
      auto &allocation = allocations_[index];
      allocation.observed = true;
      result.allocation_status_ids.push_back(allocation.status_id);
    }
    return result;
  }

  [[nodiscard]] std::vector<AllocationMaterialization> lookup(
      const std::uint32_t instruction, const SelectionScope scope,
      const std::source_location &location) const {
    std::vector<AllocationMaterialization> result;
    if (!enabled_)
      return result;
    const auto source = stable_source_path(location.file_name());
    const auto range = materialization_lines_.equal_range(location.line());
    for (auto iterator = range.first; iterator != range.second; ++iterator) {
      const auto &candidate = materializations_[iterator->second];
      if (candidate_matches(instruction, scope, source, candidate))
        result.push_back(candidate.record);
    }
    if (!result.empty())
      return result;

    constexpr std::uint_least32_t maximum_source_line_drift = 8U;
    std::optional<std::uint_least32_t> nearest_distance;
    std::optional<std::uint_least32_t> nearest_line;
    bool ambiguous_nearest_line = false;
    for (const auto &candidate : materializations_) {
      if (!candidate_matches(instruction, scope, source, candidate))
        continue;
      const auto line = candidate.record.source_line;
      const auto distance = line > location.line() ? line - location.line()
                                                   : location.line() - line;
      if (distance > maximum_source_line_drift ||
          (nearest_distance && distance > *nearest_distance))
        continue;
      if (!nearest_distance || distance < *nearest_distance) {
        nearest_distance = distance;
        nearest_line = line;
        ambiguous_nearest_line = false;
        result.clear();
      } else if (line != *nearest_line) {
        ambiguous_nearest_line = true;
      }
      if (!ambiguous_nearest_line && line == *nearest_line)
        result.push_back(candidate.record);
    }
    if (ambiguous_nearest_line)
      result.clear();
    return result;
  }

private:
  void configure() {
    const auto *native_evidence =
        std::getenv("UML_NATIVE_HARDWARE_EVIDENCE");
    if (native_evidence == nullptr || native_evidence[0] == '\0')
      return;
    const auto *evidence = std::getenv("UML_INSTRUCTION_EVIDENCE");
    if (evidence == nullptr || evidence[0] == '\0')
      return;
    const auto *test = std::getenv("UML_INSTRUCTION_EVIDENCE_TEST");
    const auto *bindings =
        std::getenv("UML_INSTRUCTION_EVIDENCE_BINDINGS");
    if (test == nullptr || test[0] == '\0' || bindings == nullptr ||
        bindings[0] == '\0')
      configuration_error("evidence test or catalog root is unset");
    enabled_ = true;
    test_name_ = test;
    const auto directory = std::filesystem::path{bindings}.parent_path();
    const auto required_path =
        directory / "a64_instruction_required_variants.tsv";
    const auto aggregate_path =
        directory / "a64_instruction_aggregate_manifest.tsv";

    std::ifstream required_input{required_path};
    std::string line;
    if (!required_input || !std::getline(required_input, line))
      configuration_error("cannot read required allocation catalog");
    const auto required_columns = columns_for(line);
    std::vector<RequiredAllocation> required;
    while (std::getline(required_input, line)) {
      if (line.empty())
        configuration_error("empty required allocation row");
      const auto fields = split_tsv(line);
      const AllocationIdentity identity{
          .stable_id = field(fields, required_columns, "stable_id"),
          .architecture_file =
              field(fields, required_columns, "architecture_file"),
          .architecture_encoding =
              field(fields, required_columns, "architecture_encoding"),
          .architecture_mask =
              field(fields, required_columns, "architecture_mask"),
          .architecture_value =
              field(fields, required_columns, "architecture_value"),
          .architecture_constraints =
              field(fields, required_columns, "architecture_constraints"),
          .decode_class = field(fields, required_columns, "decode_class"),
          .decode_encoding =
              field(fields, required_columns, "decode_encoding"),
          .decode_mask = field(fields, required_columns, "decode_mask"),
          .decode_value = field(fields, required_columns, "decode_value"),
          .decode_constraints =
              field(fields, required_columns, "decode_constraints"),
      };
      auto architecture_constraints =
          parse_constraints(identity.architecture_constraints);
      auto decode_constraints = parse_constraints(identity.decode_constraints);
      if (!architecture_constraints || !decode_constraints)
        configuration_error("invalid allocation constraint");
      required.push_back({
          .status_id = allocation_status_id(identity),
          .group = group_key(
              identity.stable_id, identity.architecture_file,
              identity.architecture_encoding, identity.architecture_mask,
              identity.architecture_value, identity.architecture_constraints,
              field(fields, required_columns, "allocation_kind"),
              identity.decode_class, identity.decode_encoding,
              identity.decode_mask, identity.decode_value,
              identity.decode_constraints),
          .architecture_mask = parse_unsigned<std::uint32_t>(
              identity.architecture_mask, 16, "invalid architecture mask"),
          .architecture_value = parse_unsigned<std::uint32_t>(
              identity.architecture_value, 16, "invalid architecture value"),
          .architecture_constraints =
              std::move(*architecture_constraints),
          .decode_mask = parse_unsigned<std::uint32_t>(
              identity.decode_mask, 16, "invalid decode mask"),
          .decode_value = parse_unsigned<std::uint32_t>(
              identity.decode_value, 16, "invalid decode value"),
          .decode_constraints = std::move(*decode_constraints),
      });
    }
    if (!required_input.eof() || required.size() != official_allocation_count)
      configuration_error("required allocation model is incomplete");
    std::map<std::string, std::size_t, std::less<>> required_by_group;
    for (std::size_t index = 0; index < required.size(); ++index)
      if (!required_by_group.emplace(required[index].group, index).second)
        configuration_error("duplicate required allocation identity");

    const auto decode_path =
        directory / "a64_instruction_decode_partitions.tsv";
    std::ifstream decode_input{decode_path};
    if (!decode_input || !std::getline(decode_input, line))
      configuration_error("cannot read decode partition catalog");
    const auto decode_columns = columns_for(line);
    std::map<std::string, DecodePartition, std::less<>> decode_partitions;
    while (std::getline(decode_input, line)) {
      if (line.empty())
        configuration_error("empty decode partition row");
      const auto fields = split_tsv(line);
      DecodePartition partition{
          .decode_class =
              std::string{field(fields, decode_columns, "decode_class")},
          .decode_encoding =
              std::string{field(fields, decode_columns, "partition_name")},
          .decode_mask = std::string{
              field(fields, decode_columns, "architecture_mask")},
          .decode_value = std::string{
              field(fields, decode_columns, "architecture_value")},
          .decode_constraints = std::string{
              field(fields, decode_columns, "architecture_constraints")},
      };
      if (!decode_partitions
               .emplace(std::string{
                            field(fields, decode_columns, "partition_id")},
                        std::move(partition))
               .second)
        configuration_error("duplicate decode partition identity");
    }
    if (!decode_input.eof())
      configuration_error("cannot finish decode partition catalog");

    std::ifstream aggregate_input{aggregate_path};
    if (!aggregate_input || !std::getline(aggregate_input, line))
      configuration_error("cannot read aggregate evidence catalog");
    const auto aggregate_columns = columns_for(line);
    std::map<std::string, SourceChoice> choices;
    std::map<std::string, bool, std::less<>> materialization_ids;
    const auto guest_page_size = std::to_string(DARWIN_UML_GUEST_PAGE_SIZE);
    while (std::getline(aggregate_input, line)) {
      if (line.empty())
        configuration_error("empty aggregate evidence row");
      const auto fields = split_tsv(line);
      if (field(fields, aggregate_columns, "guest_page_size") !=
          guest_page_size)
        continue;
      const auto scope_text =
          field(fields, aggregate_columns, "native_requirement_scope");
      SelectionScope scope;
      if (scope_text == "instruction")
        scope = SelectionScope::instruction;
      else if (scope_text == "program")
        scope = SelectionScope::program;
      else
        configuration_error("invalid aggregate native scope");
      const auto partition = decode_partitions.find(std::string{
          field(fields, aggregate_columns, "decode_partition_id")});
      if (partition == decode_partitions.end())
        configuration_error("aggregate row lacks a decode partition");
      const auto group = group_key(
          field(fields, aggregate_columns, "stable_id"),
          field(fields, aggregate_columns, "architecture_file"),
          field(fields, aggregate_columns, "architecture_encoding"),
          field(fields, aggregate_columns, "architecture_mask"),
          field(fields, aggregate_columns, "architecture_value"),
          field(fields, aggregate_columns, "architecture_constraints"),
          field(fields, aggregate_columns, "allocation_kind"),
          partition->second.decode_class,
          partition->second.decode_encoding,
          partition->second.decode_mask,
          partition->second.decode_value,
          partition->second.decode_constraints);
      const auto required_match = required_by_group.find(group);
      if (field(fields, aggregate_columns, "validity_partition") ==
          "allocated-valid") {
        if (required_match == required_by_group.end())
          configuration_error(
              "allocated-valid aggregate lacks required allocation");
        if (field(fields, aggregate_columns, "producer") == test_name_) {
          const auto &allocation = required[required_match->second];
          AllocationMaterialization materialization{
              .aggregate_id = std::string{
                  field(fields, aggregate_columns, "aggregate_id")},
              .decode_partition_id = std::string{field(
                  fields, aggregate_columns, "decode_partition_id")},
              .allocation_status_id = allocation.status_id,
              .declared_digest = std::string{
                  field(fields, aggregate_columns, "declared_digest")},
              .observed_digest = std::string{
                  field(fields, aggregate_columns, "observed_digest")},
              .architecture_features = std::string{field(
                  fields, aggregate_columns, "semantic_native_features")},
              .producer = std::string{
                  field(fields, aggregate_columns, "producer")},
              .source_site = std::string{
                  field(fields, aggregate_columns, "source_site")},
              .source_file = std::string{
                  field(fields, aggregate_columns, "source_file")},
              .source_line = parse_unsigned<std::uint_least32_t>(
                  field(fields, aggregate_columns, "source_line"), 10,
                  "invalid materialization source line"),
              .scope = scope,
              .guest_page_size = DARWIN_UML_GUEST_PAGE_SIZE,
          };
          if (!materialization_record_is_well_formed(materialization))
            configuration_error("invalid aggregate materialization record");
          if (!materialization_ids
                   .emplace(materialization.aggregate_id, true)
                   .second)
            configuration_error("duplicate aggregate materialization ID");
          materializations_.push_back({
              .record = std::move(materialization),
              .architecture_mask = allocation.architecture_mask,
              .architecture_value = allocation.architecture_value,
              .architecture_constraints =
                  allocation.architecture_constraints,
              .decode_mask = allocation.decode_mask,
              .decode_value = allocation.decode_value,
              .decode_constraints = allocation.decode_constraints,
          });
        }
      }
      SourceChoice candidate{
          .producer =
              std::string{field(fields, aggregate_columns, "producer")},
          .source = std::string{
              field(fields, aggregate_columns, "source_file")},
          .line = parse_unsigned<std::uint_least32_t>(
              field(fields, aggregate_columns, "source_line"), 10,
              "invalid aggregate source line"),
          .scope = scope,
          .exact_state =
              field(fields, aggregate_columns, "expected_state_oracle") ==
              "exact",
          .ordinary_completion =
              field(fields, aggregate_columns, "expected_event") ==
              "system-call",
          .aggregate_id = std::string{
              field(fields, aggregate_columns, "aggregate_id")},
      };
      const auto existing = choices.find(group);
      if (existing == choices.end() ||
          better_choice(candidate, existing->second))
        choices.insert_or_assign(group, std::move(candidate));
    }
    if (!aggregate_input.eof())
      configuration_error("cannot finish aggregate evidence catalog");

    for (auto &allocation : required) {
      const auto choice = choices.find(allocation.group);
      if (choice == choices.end())
        configuration_error("allocation lacks aggregate evidence");
      if (choice->second.producer != test_name_)
        continue;
      allocations_.push_back({
          .status_id = std::move(allocation.status_id),
          .source = choice->second.source,
          .line = choice->second.line,
          .scope = choice->second.scope,
          .architecture_mask = allocation.architecture_mask,
          .architecture_value = allocation.architecture_value,
          .architecture_constraints =
              std::move(allocation.architecture_constraints),
          .decode_mask = allocation.decode_mask,
          .decode_value = allocation.decode_value,
          .decode_constraints = std::move(allocation.decode_constraints),
      });
    }
    for (std::size_t index = 0; index < allocations_.size(); ++index)
      lines_.emplace(allocations_[index].line, index);
    for (std::size_t index = 0; index < materializations_.size(); ++index)
      materialization_lines_.emplace(
          materializations_[index].record.source_line, index);
  }

  bool enabled_{};
  std::string test_name_;
  std::vector<SelectedAllocation> allocations_;
  std::multimap<std::uint_least32_t, std::size_t> lines_;
  std::vector<MaterializationCandidate> materializations_;
  std::multimap<std::uint_least32_t, std::size_t> materialization_lines_;
};

[[nodiscard]] SelectionRegistry &selection_registry() {
  static SelectionRegistry registry;
  return registry;
}

} // namespace

std::string allocation_status_id(const AllocationIdentity &identity) {
  const auto text = identity_text(identity);
  const auto characters = std::span<const char>{text.data(), text.size()};
  const auto digest = identity_digest(std::as_bytes(characters));
  constexpr char digits[] = "0123456789abcdef";
  std::string result = "behavior-status-";
  result.reserve(result.size() + 20U);
  for (std::size_t index = 0; index < 10U; ++index) {
    const auto value = std::to_integer<std::uint8_t>(digest[index]);
    result.push_back(digits[value >> 4U]);
    result.push_back(digits[value & 15U]);
  }
  return result;
}

bool allocation_encoding_matches(
    const std::uint32_t instruction, const std::uint32_t architecture_mask,
    const std::uint32_t architecture_value,
    const std::string_view architecture_constraints,
    const std::uint32_t decode_mask, const std::uint32_t decode_value,
    const std::string_view decode_constraints) noexcept {
  try {
    const auto architecture = parse_constraints(architecture_constraints);
    const auto decode = parse_constraints(decode_constraints);
    return architecture && decode &&
        (instruction & architecture_mask) == architecture_value &&
        (instruction & decode_mask) == decode_value &&
        constraints_match(instruction, *architecture) &&
        constraints_match(instruction, *decode);
  } catch (...) {
    return false;
  }
}

bool materialization_record_is_well_formed(
    const AllocationMaterialization &record) noexcept {
  const bool valid_scope = record.scope == SelectionScope::instruction ||
      record.scope == SelectionScope::program;
  return exact_prefixed_hex(record.aggregate_id, "aggregate-", 20U) &&
      exact_prefixed_hex(record.decode_partition_id, "decode-", 20U) &&
      exact_prefixed_hex(
          record.allocation_status_id, "behavior-status-", 20U) &&
      exact_lower_hex(record.declared_digest, 64U) &&
      exact_lower_hex(record.observed_digest, 64U) &&
      !record.architecture_features.empty() && !record.producer.empty() &&
      exact_lower_hex(record.source_site, 20U) &&
      record.source_file.starts_with("tests/") &&
      record.source_line != 0U && valid_scope &&
      (record.guest_page_size == 4096U ||
       record.guest_page_size == 16384U);
}

AllocationSelection select_native_allocations(
    const std::uint32_t instruction, const SelectionScope scope,
    const std::source_location &location) {
  const auto *native_evidence =
      std::getenv("UML_NATIVE_HARDWARE_EVIDENCE");
  if (native_evidence == nullptr || native_evidence[0] == '\0')
    return {};
  return selection_registry().take(instruction, scope, location);
}

std::vector<AllocationMaterialization> lookup_native_materializations(
    const std::uint32_t instruction, const SelectionScope scope,
    const std::source_location &location) {
  const auto *native_evidence =
      std::getenv("UML_NATIVE_HARDWARE_EVIDENCE");
  if (native_evidence == nullptr || native_evidence[0] == '\0')
    return {};
  return selection_registry().lookup(instruction, scope, location);
}

} // namespace uml::emulator::tests::native_oracle
