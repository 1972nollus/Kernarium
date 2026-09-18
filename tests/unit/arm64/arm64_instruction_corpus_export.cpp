// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_instruction_corpus_export.hpp"

#include "uml_darwin_guest.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if !defined(UML_ARM64_DECODE_PARTITIONS) || !defined(UML_ARM64_ALLOCATIONS)
#error "the checked decode partitions and allocations must be named"
#endif

namespace uml::emulator::tests::corpus_export {
namespace {

constexpr std::uint64_t default_seed = 0xa64c'0b05'5eed'2026ULL;
constexpr std::size_t default_sample_limit = 8;
constexpr std::array<char, 8> export_magic{'A', '6', '4', 'C', 'E', 'X', 'P',
                                           '1'};

[[noreturn]] void export_failure(const std::string &message) {
  std::fprintf(stderr, "arm64 corpus export: %s\n", message.c_str());
  std::abort();
}

[[nodiscard]] std::uint64_t fnv1a(std::string_view text,
                                  std::uint64_t hash =
                                      0xcbf2'9ce4'8422'2325ULL) noexcept {
  for (const unsigned char character : text) {
    hash ^= character;
    hash *= 0x0000'0100'0000'01b3ULL;
  }
  return hash;
}

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t &state) noexcept {
  state += 0x9e37'79b9'7f4a'7c15ULL;
  auto value = state;
  value = (value ^ (value >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d0'49bb'1331'11ebULL;
  return value ^ (value >> 31U);
}

[[nodiscard]] std::string repository_path(std::string_view path) {
  constexpr std::string_view marker{"tests/"};
  if (path.starts_with(marker))
    return std::string{path};
  if (const auto found = path.rfind("/tests/"); found != std::string_view::npos)
    return std::string{path.substr(found + 1U)};
  return std::filesystem::path{path}.filename().string();
}

[[nodiscard]] std::string site_of(const std::source_location &location) {
  return repository_path(location.file_name()) + ":" +
         std::to_string(location.line());
}

[[nodiscard]] std::string_view event_token(ExpectedEvent event) noexcept {
  switch (event) {
  case ExpectedEvent::system_call:
    return "system-call";
  case ExpectedEvent::memory_fault:
    return "memory-fault";
  case ExpectedEvent::gcs_fault:
    return "gcs-fault";
  case ExpectedEvent::illegal_instruction:
    return "illegal-instruction";
  case ExpectedEvent::breakpoint:
    return "breakpoint";
  case ExpectedEvent::floating_point_trap:
    return "floating-point-trap";
  case ExpectedEvent::pc_alignment:
    return "pc-alignment";
  case ExpectedEvent::instruction_budget:
    return "instruction-budget";
  case ExpectedEvent::fatal:
    return "fatal";
  }
  return "unknown";
}

[[nodiscard]] std::string_view access_token(arm64::MemoryAccess access) {
  switch (access) {
  case arm64::MemoryAccess::read:
    return "read";
  case arm64::MemoryAccess::write:
    return "write";
  case arm64::MemoryAccess::execute:
    return "execute";
  }
  return "unknown";
}

[[nodiscard]] std::string feature_tokens(const ArmFeatureSet &features) {
  std::string result;
  for (std::size_t index = 0; index < features.size(); ++index) {
    if (!features.test(index))
      continue;
    if (!result.empty())
      result += ',';
    result += ::uml::platform::arm_feature_tokens[index];
  }
  return result;
}

[[nodiscard]] std::vector<std::string> split_tabs(const std::string &line) {
  std::vector<std::string> fields;
  std::size_t begin = 0;
  for (;;) {
    const auto end = line.find('\t', begin);
    fields.push_back(line.substr(begin, end - begin));
    if (end == std::string::npos)
      break;
    begin = end + 1U;
  }
  return fields;
}

/* Keeps a free-form value inside one TSV column. */
[[nodiscard]] std::string tsv_field(std::string_view text) {
  std::string result{text};
  std::replace_if(
      result.begin(), result.end(),
      [](char character) { return character == '\t' || character == '\n'; },
      ' ');
  return result;
}

[[nodiscard]] std::uint64_t parse_number(const std::string &text, int base,
                                         const std::string &line) {
  std::size_t consumed = 0;
  std::uint64_t value = 0;
  try {
    value = std::stoull(text, &consumed, base);
  } catch (const std::exception &) {
    consumed = 0;
  }
  if (text.empty() || consumed != text.size())
    export_failure("malformed number in force-include directive: " + line);
  return value;
}

/* The operand fields of a decode pattern are the bits its mask leaves free: a
 * five-bit register field at each standard A64 register position (bits 0, 5,
 * 10, and 16) that is entirely free, and each maximal run of free bits
 * elsewhere. A field's boundary values are 0, 30, and 31 for a register, 0 and
 * 1 for a single bit, and 0 and all ones for a wider field. */
struct OperandField final {
  unsigned low{};
  unsigned width{};
  bool register_field{};
};

[[nodiscard]] std::vector<OperandField> operand_fields(std::uint32_t mask) {
  std::vector<OperandField> fields;
  auto open = ~mask;
  for (const unsigned low : {0U, 5U, 10U, 16U}) {
    const auto bits = 31U << low;
    if ((open & bits) == bits) {
      fields.push_back({.low = low, .width = 5U, .register_field = true});
      open &= ~bits;
    }
  }
  for (unsigned bit = 0; bit < 32U;) {
    if (((open >> bit) & 1U) == 0U) {
      ++bit;
      continue;
    }
    const auto low = bit;
    while (bit < 32U && ((open >> bit) & 1U) != 0U)
      ++bit;
    fields.push_back({.low = low, .width = bit - low, .register_field = false});
  }
  return fields;
}

[[nodiscard]] bool boundary_value(const OperandField &field,
                                  std::uint32_t value) noexcept {
  if (field.register_field)
    return value == 0U || value == 30U || value == 31U;
  const auto top = field.width >= 32U ? ~std::uint32_t{0}
                                      : (std::uint32_t{1} << field.width) - 1U;
  return value == 0U || value == top;
}

/* A mask-bucketed copy of the official A64 decode hierarchy. A word selects
 * exactly one hierarchy leaf; an allocated class then selects exactly one row
 * of its instruction table. Unmatched words keep an explicit sentinel. The
 * official allocation predicates of the inventory additionally separate alias
 * subsets, such as SXTB within SBFM, into their own partition. */
class DecodePartitions final {
public:
  DecodePartitions(const std::filesystem::path &partitions,
                   const std::filesystem::path &allocations) {
    for_each_row(partitions, [&](const auto &column) {
      Pattern pattern{
          .id = column("partition_id"),
          .decode_class = column("decode_class"),
          .unallocated = column("partition_kind") == "unallocated",
          .mask = static_cast<std::uint32_t>(
              std::stoul(column("architecture_mask"), nullptr, 16)),
          .value = static_cast<std::uint32_t>(
              std::stoul(column("architecture_value"), nullptr, 16)),
          .constraints =
              parse_constraints(column("architecture_constraints")),
      };
      const auto &layer = column("decode_layer");
      if (layer == "hierarchy") {
        const auto index = hierarchy_.size();
        hierarchy_.push_back(std::move(pattern));
        add_to_buckets(hierarchy_buckets_, hierarchy_[index], index);
      } else if (layer == "instruction-table") {
        tables_[pattern.decode_class].push_back(std::move(pattern));
      } else {
        export_failure("unknown decode layer " + layer);
      }
    });
    for_each_row(allocations, [&](const auto &column) {
      const auto index = allocations_.size();
      allocations_.push_back({
          .id = column("stable_id"),
          .mask = static_cast<std::uint32_t>(
              std::stoul(column("architecture_mask"), nullptr, 16)),
          .value = static_cast<std::uint32_t>(
              std::stoul(column("architecture_value"), nullptr, 16)),
          .constraints =
              parse_constraints(column("architecture_constraints")),
      });
      add_to_buckets(allocation_buckets_, allocations_[index], index);
    });
  }

  [[nodiscard]] const std::string &classify(std::uint32_t word) {
    return classified(word).key;
  }

  /* Identities of the (partition, operand field, boundary value) features of
   * one word; empty for a word outside every partition. */
  [[nodiscard]] const std::vector<std::uint64_t> &
  boundary_features(std::uint32_t word) {
    return classified(word).boundaries;
  }

private:
  struct Classified final {
    std::string key;
    std::vector<std::uint64_t> boundaries;
  };

  [[nodiscard]] const Classified &classified(std::uint32_t word) {
    if (const auto found = cache_.find(word); found != cache_.end())
      return found->second;
    const Pattern *leaf = nullptr;
    unsigned leaves = 0;
    for (const auto index : hierarchy_buckets_[word >> 24U])
      if (hierarchy_[index].matches(word)) {
        leaf = &hierarchy_[index];
        ++leaves;
      }
    std::string result = unmatched_;
    const Pattern *pattern = nullptr;
    if (leaves == 1U) {
      if (leaf->unallocated) {
        pattern = leaf;
      } else if (const auto table = tables_.find(leaf->decode_class);
                 table != tables_.end()) {
        const Pattern *row = nullptr;
        unsigned rows = 0;
        for (const auto &candidate : table->second)
          if (candidate.matches(word)) {
            row = &candidate;
            ++rows;
          }
        if (rows == 1U)
          pattern = row;
      }
    }
    if (pattern != nullptr)
      result = pattern->id;
    std::vector<std::string> aliases;
    for (const auto index : allocation_buckets_[word >> 24U])
      if (allocations_[index].matches(word))
        aliases.push_back(allocations_[index].id);
    std::sort(aliases.begin(), aliases.end());
    for (const auto &alias : aliases)
      result += "+" + alias;
    Classified entry{.key = std::move(result), .boundaries = {}};
    if (pattern != nullptr) {
      const auto partition = fnv1a(entry.key);
      for (const auto &field : operand_fields(pattern->mask)) {
        const auto value =
            (word >> field.low) &
            (field.width >= 32U ? ~std::uint32_t{0}
                                : (std::uint32_t{1} << field.width) - 1U);
        if (boundary_value(field, value))
          entry.boundaries.push_back(fnv1a(
              std::to_string(field.low) + ":" + std::to_string(field.width) +
                  "=" + std::to_string(value),
              partition));
      }
    }
    return cache_.emplace(word, std::move(entry)).first->second;
  }

  struct Constraint final {
    unsigned low{};
    unsigned width{};
    bool equal{};
    std::string pattern;
  };

  struct Pattern final {
    std::string id;
    std::string decode_class;
    bool unallocated{};
    std::uint32_t mask{};
    std::uint32_t value{};
    std::vector<Constraint> constraints;

    [[nodiscard]] bool matches(std::uint32_t word) const noexcept {
      if ((word & mask) != value)
        return false;
      for (const auto &constraint : constraints) {
        bool same = true;
        for (unsigned bit = 0; bit < constraint.width; ++bit) {
          const char expected = constraint.pattern[bit];
          const unsigned position = constraint.low + constraint.width - 1U - bit;
          const char actual = ((word >> position) & 1U) != 0U ? '1' : '0';
          if (expected != 'x' && expected != actual) {
            same = false;
            break;
          }
        }
        if (same != constraint.equal)
          return false;
      }
      return true;
    }
  };

  template <typename Visitor>
  static void for_each_row(const std::filesystem::path &path,
                           Visitor &&visit) {
    std::ifstream input{path};
    if (!input)
      export_failure("cannot read " + path.string());
    std::string line;
    if (!std::getline(input, line))
      export_failure(path.string() + " is empty");
    const auto header = split_tabs(line);
    while (std::getline(input, line)) {
      if (line.empty())
        continue;
      const auto fields = split_tabs(line);
      if (fields.size() != header.size())
        export_failure("malformed row in " + path.string());
      visit([&](std::string_view name) -> const std::string & {
        const auto found = std::find(header.begin(), header.end(), name);
        if (found == header.end())
          export_failure(path.string() + " lacks column " +
                         std::string{name});
        return fields[static_cast<std::size_t>(found - header.begin())];
      });
    }
  }

  static void add_to_buckets(std::array<std::vector<std::size_t>, 256> &buckets,
                             const Pattern &pattern, std::size_t index) {
    for (unsigned bucket = 0; bucket < 256U; ++bucket) {
      const auto top = static_cast<std::uint32_t>(bucket) << 24U;
      if ((top & pattern.mask & 0xff00'0000U) ==
          (pattern.value & 0xff00'0000U))
        buckets[bucket].push_back(index);
    }
  }

  [[nodiscard]] static std::vector<Constraint>
  parse_constraints(const std::string &text) {
    std::vector<Constraint> result;
    if (text == "none")
      return result;
    std::size_t begin = 0;
    for (;;) {
      const auto end = text.find(';', begin);
      const auto item = text.substr(begin, end - begin);
      const auto at = item.find('@');
      const auto colon = item.find(':', at);
      auto operation = item.find("!=", colon);
      bool equal = false;
      std::size_t operator_size = 2;
      if (operation == std::string::npos) {
        operation = item.find("==", colon);
        equal = true;
      }
      if (operation == std::string::npos) {
        operation = item.find('=', colon);
        operator_size = 1;
        equal = true;
      }
      if (at == std::string::npos || colon == std::string::npos ||
          operation == std::string::npos)
        export_failure("unsupported decode constraint " + item);
      Constraint constraint{
          .low = static_cast<unsigned>(
              std::stoul(item.substr(at + 1U, colon - at - 1U))),
          .width = static_cast<unsigned>(
              std::stoul(item.substr(colon + 1U, operation - colon - 1U))),
          .equal = equal,
          .pattern = item.substr(operation + operator_size),
      };
      if (constraint.pattern.size() != constraint.width)
        export_failure("decode constraint width mismatch " + item);
      result.push_back(std::move(constraint));
      if (end == std::string::npos)
        break;
      begin = end + 1U;
    }
    return result;
  }

  std::vector<Pattern> hierarchy_;
  std::array<std::vector<std::size_t>, 256> hierarchy_buckets_;
  std::unordered_map<std::string, std::vector<Pattern>> tables_;
  std::vector<Pattern> allocations_;
  std::array<std::vector<std::size_t>, 256> allocation_buckets_;
  std::unordered_map<std::uint32_t, Classified> cache_;
  std::string unmatched_{"unmatched"};
};

class Writer final {
public:
  void u8(std::uint8_t value) { bytes_.push_back(static_cast<char>(value)); }
  void u16(std::uint16_t value) { little(value, 2); }
  void u32(std::uint32_t value) { little(value, 4); }
  void u64(std::uint64_t value) { little(value, 8); }
  void boolean(bool value) { u8(value ? 1U : 0U); }
  void string(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    bytes_.append(value.data(), value.size());
  }
  void bytes(std::span<const std::byte> value) {
    bytes_.append(reinterpret_cast<const char *>(value.data()), value.size());
  }
  [[nodiscard]] const std::string &data() const noexcept { return bytes_; }
  void clear() noexcept { bytes_.clear(); }

private:
  void little(std::uint64_t value, unsigned size) {
    for (unsigned index = 0; index < size; ++index)
      bytes_.push_back(static_cast<char>((value >> (index * 8U)) & 0xffU));
  }

  std::string bytes_;
};

/* The field order is the export contract read by build-unit-corpus.py. */
void write_state(Writer &writer, const arm64::CpuState &state) {
  for (const auto value : state.registers)
    writer.u64(value);
  writer.u64(state.sp);
  writer.u64(state.pc);
  writer.u32(state.nzcv);
  for (const auto &vector : state.vectors)
    writer.bytes(vector.bytes);
  writer.u32(state.fpcr);
  writer.u32(state.fpsr);
  writer.u64(state.fpmr);
  writer.u64(state.tls);
  writer.boolean(state.mte_enabled);
  writer.boolean(state.tbi_enabled);
  writer.u16(state.mte_exclude);
  for (const auto choice : state.mte_tag_choices)
    writer.u8(choice);
  writer.boolean(state.mte_random_tags);
  writer.u16(state.mte_seed);
  writer.u8(state.mte_tag);
  writer.u64(state.mte_random_state);
  writer.boolean(state.cpa_enabled);
  writer.boolean(state.cpa_multiply_enabled);
  writer.u32(state.accdata);
  writer.boolean(state.pstate_ssbs);
  writer.boolean(state.pstate_dit);
  writer.boolean(state.pstate_tco);
  writer.boolean(state.pstate_sm);
  writer.boolean(state.pstate_za);
  writer.boolean(state.pstate_daif_access);
  writer.u8(state.pstate_daif);
  writer.u64(state.exclusive_address);
  writer.u64(static_cast<std::uint64_t>(state.exclusive_key));
  writer.u64(state.exclusive_value);
  writer.u64(state.exclusive_value2);
  writer.u64(state.exclusive_version);
  writer.u8(state.exclusive_size);
  writer.boolean(state.fp_dirty);
  writer.u64(state.gcs_pointer);
  writer.boolean(state.gcs_implemented);
  writer.boolean(state.gcs_push_enabled);
  writer.boolean(state.gcs_store_enabled);
  for (const auto &key : state.pointer_auth_keys) {
    writer.u64(key.low);
    writer.u64(key.high);
  }
  writer.boolean(state.pointer_authentication_enabled);
}

struct ClassRecord final {
  std::uint64_t count{};
  std::string first_case;
  std::string site;
  /* Boundary features of the cases selected for covering them, and how many
   * cases that selected. */
  std::unordered_set<std::uint64_t> boundaries;
  std::uint64_t covering{};
  /* Sample pass only. */
  std::uint64_t expected{};
  std::uint64_t expected_covering{};
  std::vector<std::uint64_t> selected;
  std::size_t next_selected{};
};

[[nodiscard]] std::vector<std::uint64_t> select_indices(std::uint64_t size,
                                                        std::size_t limit,
                                                        std::uint64_t seed) {
  std::vector<std::uint64_t> result;
  if (size <= limit) {
    result.resize(size);
    for (std::uint64_t index = 0; index < size; ++index)
      result[index] = index;
    return result;
  }
  std::set<std::uint64_t> chosen{0, size - 1U};
  auto state = seed;
  while (chosen.size() < limit)
    chosen.insert(1U + splitmix64(state) % (size - 2U));
  result.assign(chosen.begin(), chosen.end());
  return result;
}

[[nodiscard]] std::string retirement_key(const ProgramCase &test_case) {
  if (test_case.expected_instructions.has_value())
    return "exact:" + std::to_string(*test_case.expected_instructions);
  if (test_case.expected_instruction_pattern.has_value()) {
    const auto &pattern = *test_case.expected_instruction_pattern;
    return "pattern:" + std::to_string(pattern.minimum) + ":" +
           std::to_string(pattern.step) + ":" +
           (pattern.maximum.has_value() ? std::to_string(*pattern.maximum)
                                        : std::string{"unbounded"});
  }
  return test_case.event == ExpectedEvent::instruction_budget
             ? "unchecked"
             : "interpreter";
}

[[nodiscard]] unsigned register31_fields(std::uint32_t word) noexcept {
  unsigned mask = 0;
  constexpr std::array<unsigned, 4> shifts{0U, 5U, 10U, 16U};
  for (unsigned index = 0; index < shifts.size(); ++index)
    if (((word >> shifts[index]) & 31U) == 31U)
      mask |= 1U << index;
  return mask;
}

} // namespace

class Exporter::State final {
public:
  State()
      : directory_{std::getenv("UML_ARM64_CORPUS_EXPORT")},
        creator_{::getpid()} {
    const auto *pass = std::getenv("UML_ARM64_CORPUS_EXPORT_PASS");
    const std::string_view pass_text = pass == nullptr ? "" : pass;
    if (pass_text == "count")
      sampling_ = false;
    else if (pass_text == "sample")
      sampling_ = true;
    else
      export_failure("UML_ARM64_CORPUS_EXPORT_PASS must be count or sample");
    const auto *producer = std::getenv("UML_INSTRUCTION_EVIDENCE_TEST");
    if (producer == nullptr || producer[0] == '\0')
      export_failure("the CTest producer name is unavailable");
    producer_ = producer;
    if (const auto *limit = std::getenv("UML_ARM64_CORPUS_K");
        limit != nullptr && limit[0] != '\0')
      limit_ = static_cast<std::size_t>(std::stoull(limit));
    if (limit_ < 2U)
      export_failure("the class sample limit must keep first and last cases");
    if (const auto *seed = std::getenv("UML_ARM64_CORPUS_SEED");
        seed != nullptr && seed[0] != '\0')
      seed_ = std::stoull(seed, nullptr, 0);
    partitions_ = std::make_unique<DecodePartitions>(
        UML_ARM64_DECODE_PARTITIONS, UML_ARM64_ALLOCATIONS);
    std::filesystem::create_directories(directory_);
    if (sampling_) {
      load_counts();
      if (const auto *include = std::getenv("UML_ARM64_CORPUS_INCLUDE");
          include != nullptr && include[0] != '\0')
        load_directives(include);
    }
  }

  ~State() {
    if (::getpid() != creator_)
      return;
    finish();
  }

  Selection observe(const ProgramCase &test_case,
                    const EngineHarnessOptions &options,
                    const std::source_location &location,
                    std::uint64_t ordinal) {
    std::scoped_lock lock{mutex_};
    Selection selection;
    selection.ordinal = ordinal;
    selection.site = site_of(location);
    selection.class_key = class_key(test_case, options, selection.site);
    ++observed_;
    order_ = splitmix_combine(
        order_, std::hash<std::string>{}(selection.class_key) ^ ordinal);
    if (!sampling_) {
      auto &record = classes_[selection.class_key];
      if (record.count++ == 0U) {
        record.first_case = test_case.name;
        record.site = selection.site;
      }
      if (covers_boundary(record, test_case))
        ++record.covering;
      return selection;
    }
    const auto found = classes_.find(selection.class_key);
    if (found == classes_.end()) {
      determinism_failures_.push_back("unknown class " + selection.class_key);
      return selection;
    }
    auto &record = found->second;
    selection.index = record.count++;
    selection.size = record.expected;
    if (record.next_selected < record.selected.size() &&
        record.selected[record.next_selected] == selection.index) {
      ++record.next_selected;
      selection.sampled = true;
    }
    if (covers_boundary(record, test_case)) {
      ++record.covering;
      selection.covering = true;
    }
    if (const auto directive =
            force_directive(test_case, ordinal, &selection.class_key);
        !directive.empty()) {
      selection.forced = true;
      forced_report_.push_back(directive + "\t" + std::to_string(ordinal) +
                               "\t" + tsv_field(test_case.name) +
                               "\texported");
    }
    selection.selected =
        selection.sampled || selection.covering || selection.forced;
    return selection;
  }

  void exclude(const ProgramCase &test_case, std::string_view reason,
               const std::source_location &location, std::uint64_t ordinal) {
    std::scoped_lock lock{mutex_};
    ++exclusions_[std::string{reason} + "\t" + site_of(location)];
    if (!sampling_)
      return;
    if (const auto directive = force_directive(test_case, ordinal, nullptr);
        !directive.empty())
      forced_report_.push_back(directive + "\t" + std::to_string(ordinal) +
                               "\t" + tsv_field(test_case.name) +
                               "\texcluded:" + std::string{reason});
  }

  void write(const ExportCase &test_case, const Selection &selection,
             const EngineHarnessOptions &options, const ExportResult &result) {
    std::scoped_lock lock{mutex_};
    open_stream();
    Writer writer;
    writer.u8(1);
    writer.string(test_case.name);
    writer.string(selection.site);
    writer.string(selection.class_key);
    writer.u64(selection.index);
    writer.u64(selection.size);
    writer.u64(selection.ordinal);
    writer.u8(static_cast<std::uint8_t>((selection.sampled ? 1U : 0U) |
                                        (selection.forced ? 2U : 0U) |
                                        (selection.covering ? 4U : 0U)));
    writer.u32(option_mask(options));
    writer.u8(static_cast<std::uint8_t>(test_case.event));
    writer.u8(static_cast<std::uint8_t>(
        (test_case.invalid_encoding ? 1U : 0U) |
        (test_case.compare_fault ? 2U : 0U) |
        (test_case.validator != nullptr ? 4U : 0U)));
    writer.u8(static_cast<std::uint8_t>(result.minimization));
    if (test_case.expected_instructions.has_value()) {
      writer.u8(0);
      writer.u64(*test_case.expected_instructions);
    } else if (test_case.expected_instruction_pattern.has_value()) {
      const auto &pattern = *test_case.expected_instruction_pattern;
      writer.u8(1);
      writer.u64(pattern.minimum);
      writer.u64(pattern.step);
      writer.boolean(pattern.maximum.has_value());
      writer.u64(pattern.maximum.value_or(0));
    } else if (test_case.event == ExpectedEvent::instruction_budget) {
      writer.u8(2);
    } else {
      if (!result.interpreter_instructions.has_value())
        export_failure("interpreter retirement is missing for " +
                       test_case.name);
      writer.u8(0);
      writer.u64(*result.interpreter_instructions);
    }
    writer.u64(test_case.fault.address);
    writer.u8(static_cast<std::uint8_t>(test_case.fault.access));
    writer.u32(test_case.syndrome);
    writer.string(feature_tokens(test_case.native.features()));
    writer.string(test_case.validator_relation_id);
    writer.u32(static_cast<std::uint32_t>(test_case.entry_offset));
    writer.u32(static_cast<std::uint32_t>(test_case.code.size()));
    for (const auto word : test_case.code) {
      writer.u32(static_cast<std::uint32_t>(word.offset));
      writer.u32(word.instruction);
    }
    auto initial = test_case.initial;
    initial.pc = engine_test_code_base + test_case.entry_offset;
    write_state(writer, initial);
    write_state(writer, test_case.expected);
    writer.u32(static_cast<std::uint32_t>(test_case.memory.size()));
    for (const auto &region : test_case.memory) {
      writer.u64(region.address);
      writer.u32(static_cast<std::uint32_t>(region.initial.size()));
      writer.u8(static_cast<std::uint8_t>(
          (region.readable ? 1U : 0U) | (region.writable ? 2U : 0U) |
          (region.executable ? 4U : 0U) | (region.guest_code ? 8U : 0U) |
          (region.guest_writable ? 16U : 0U) |
          (!region.expected.empty() ? 32U : 0U)));
      writer.bytes(region.initial);
      if (!region.expected.empty())
        writer.bytes(region.expected);
      for (const auto *tags : {&region.initial_tags, &region.expected_tags}) {
        writer.u32(static_cast<std::uint32_t>(tags->size()));
        for (const auto tag : *tags) {
          writer.u32(static_cast<std::uint32_t>(tag.offset));
          writer.u8(tag.value);
        }
      }
    }
    writer.u64(result.interpreter_instructions.value_or(~std::uint64_t{0}));
    writer.u8(static_cast<std::uint8_t>(
        (result.interpreter ? 1U : 0U) | (result.gadgets ? 2U : 0U) |
        (result.jit ? 4U : 0U) | (result.jit_skipped_host_feature ? 8U : 0U)));
    append(writer.data());
    ++written_;
    ++minimized_[static_cast<std::size_t>(result.minimization)];
    constexpr std::array<std::string_view, 3> levels{"full", "narrow",
                                                     "broad"};
    std::string reasons;
    for (const auto &[flag, reason] :
         {std::pair{selection.sampled, "sampled"},
          std::pair{selection.covering, "covering"},
          std::pair{selection.forced, "forced"}})
      if (flag)
        reasons += (reasons.empty() ? "" : "+") + std::string{reason};
    case_index_.push_back(
        std::to_string(selection.ordinal) + "\t" + tsv_field(test_case.name) +
        "\t" + selection.site + "\t" + reasons + "\t" +
        std::string{levels[static_cast<std::size_t>(result.minimization)]});
  }

private:
  [[nodiscard]] std::string class_key(const ProgramCase &test_case,
                                      const EngineHarnessOptions &options,
                                      const std::string &site) {
    std::vector<ProgramWord> words{test_case.code.begin(),
                                   test_case.code.end()};
    std::sort(words.begin(), words.end(),
              [](const ProgramWord &left, const ProgramWord &right) {
                return left.offset < right.offset;
              });
    std::ostringstream key;
    key << "site=" << site << "|event=" << event_token(test_case.event)
        << "|invalid=" << (test_case.invalid_encoding ? 1 : 0)
        << "|options=" << std::hex << option_mask(options) << std::dec
        << "|retire=" << retirement_key(test_case) << "|fault="
        << (test_case.compare_fault ? access_token(test_case.fault.access)
                                    : std::string_view{"none"})
        << "|validator="
        << (test_case.validator == nullptr
                ? std::string_view{"exact"}
                : test_case.validator_relation_id)
        << "|native=" << feature_tokens(test_case.native.features())
        << "|entry=" << test_case.entry_offset << "|words=";
    bool first = true;
    for (const auto word : words) {
      if (!first)
        key << ',';
      first = false;
      key << partitions_->classify(word.instruction) << '/'
          << register31_fields(word.instruction);
    }
    return key.str();
  }

  /* Selects a case that gives some operand field of some program word a
   * boundary value no earlier covering case of its class gave it. */
  [[nodiscard]] bool covers_boundary(ClassRecord &record,
                                     const ProgramCase &test_case) {
    bool covering = false;
    for (const auto word : test_case.code)
      for (const auto feature :
           partitions_->boundary_features(word.instruction))
        covering |= !record.boundaries.contains(feature);
    if (covering)
      for (const auto word : test_case.code)
        for (const auto feature :
             partitions_->boundary_features(word.instruction))
          record.boundaries.insert(feature);
    return covering;
  }

  /* Returns the directive that forces this case, or an empty string. */
  [[nodiscard]] std::string force_directive(const ProgramCase &test_case,
                                            std::uint64_t ordinal,
                                            const std::string *class_key) {
    if (const auto found = forced_ordinals_.find(ordinal);
        found != forced_ordinals_.end()) {
      seen_ordinals_.insert(ordinal);
      return found->second;
    }
    for (auto &directive : word_directives_) {
      const bool matches = std::ranges::any_of(
          test_case.code, [&](const ProgramWord word) {
            return (word.instruction & directive.mask) == directive.value;
          });
      if (!matches)
        continue;
      if (directive.limit != 0U && class_key != nullptr &&
          directive.forced_per_class[*class_key]++ >= directive.limit)
        continue;
      return directive.text;
    }
    return {};
  }

  void load_directives(const char *file) {
    std::ifstream input{file};
    if (!input)
      export_failure(std::string{"cannot read "} + file);
    std::string line;
    while (std::getline(input, line)) {
      if (line.empty() || line.front() == '#')
        continue;
      const auto fields = split_tabs(line);
      const auto text = tsv_field(line);
      if (fields[0] == "case" && fields.size() == 3U) {
        const auto ordinal = parse_number(fields[2], 10, line);
        if (ordinal == 0U)
          export_failure("check ordinals start at 1: " + line);
        if (fields[1] == producer_)
          forced_ordinals_.emplace(ordinal, text);
      } else if (fields[0] == "word" && fields.size() >= 3U &&
                 fields.size() <= 5U) {
        const auto mask = parse_number(fields[1], 16, line);
        const auto value = parse_number(fields[2], 16, line);
        if (mask > 0xffff'ffffU || (value & ~mask) != 0U)
          export_failure("word value must lie inside a 32-bit mask: " + line);
        if (fields.size() >= 4U && fields[3] != "*" && fields[3] != producer_)
          continue;
        word_directives_.push_back({
            .mask = static_cast<std::uint32_t>(mask),
            .value = static_cast<std::uint32_t>(value),
            .limit = fields.size() == 5U ? parse_number(fields[4], 10, line)
                                         : 0U,
            .text = text,
            .forced_per_class = {},
        });
      } else {
        export_failure("malformed force-include directive: " + line);
      }
    }
    directives_ = !forced_ordinals_.empty() || !word_directives_.empty();
  }

  [[nodiscard]] static std::uint64_t
  splitmix_combine(std::uint64_t state, std::uint64_t value) noexcept {
    state ^= value;
    return splitmix64(state);
  }

  [[nodiscard]] std::filesystem::path path(std::string_view suffix) const {
    return std::filesystem::path{directory_} / (producer_ + std::string{suffix});
  }

  void load_counts() {
    std::ifstream input{path(".classes.tsv")};
    if (!input)
      export_failure("the count pass output is missing for " + producer_);
    std::string line;
    if (!std::getline(input, line) ||
        line != "class\tcount\tcovering\tsite\tfirst_case")
      export_failure("the count pass output has an unexpected header");
    while (std::getline(input, line)) {
      const auto first = line.find('\t');
      const auto second = line.find('\t', first + 1U);
      const auto third = second == std::string::npos
                             ? std::string::npos
                             : line.find('\t', second + 1U);
      if (first == std::string::npos || third == std::string::npos)
        export_failure("malformed count pass row");
      const auto key = line.substr(0, first);
      auto &record = classes_[key];
      record.expected = std::stoull(line.substr(first + 1U, second - first - 1U));
      record.expected_covering =
          std::stoull(line.substr(second + 1U, third - second - 1U));
      record.selected = select_indices(
          record.expected, limit_,
          fnv1a(key, fnv1a(producer_ + "\n", seed_)));
    }
    std::ifstream order{path(".order")};
    std::string observed_label, order_label;
    if (!(order >> observed_label >> expected_observed_ >> order_label >>
          std::hex >> expected_order_) ||
        observed_label != "observed" || order_label != "order")
      export_failure("the count pass order record is missing for " +
                     producer_);
  }

  void open_stream() {
    if (stream_ != nullptr)
      return;
    const auto target = path(".cases.gz.tmp");
    stream_ = gzopen(target.c_str(), "wb6");
    if (stream_ == nullptr)
      export_failure("cannot open " + target.string());
    Writer header;
    for (const auto character : export_magic)
      header.u8(static_cast<std::uint8_t>(character));
    header.u32(DARWIN_UML_GUEST_PAGE_SIZE);
    header.u32(static_cast<std::uint32_t>(limit_));
    header.u64(seed_);
    header.string(producer_);
    append(header.data());
  }

  void append(const std::string &bytes) {
    if (bytes.empty())
      return;
    if (gzwrite(stream_, bytes.data(), static_cast<unsigned>(bytes.size())) !=
        static_cast<int>(bytes.size()))
      export_failure("cannot write the export stream");
  }

  void finish() {
    std::scoped_lock lock{mutex_};
    if (!sampling_) {
      const auto target = path(".classes.tsv");
      const auto temporary = path(".classes.tsv.tmp");
      {
        std::ofstream output{temporary, std::ios::trunc};
        output << "class\tcount\tcovering\tsite\tfirst_case\n";
        for (const auto &[key, record] : classes_)
          output << key << '\t' << record.count << '\t' << record.covering
                 << '\t' << record.site << '\t' << record.first_case << '\n';
        if (!output)
          export_failure("cannot write " + temporary.string());
      }
      std::filesystem::rename(temporary, target);
      std::ofstream order{path(".order"), std::ios::trunc};
      order << "observed\t" << observed_ << "\norder\t" << std::hex << order_
            << '\n';
      if (!order)
        export_failure("cannot write the order record for " + producer_);
    } else {
      for (const auto &[key, record] : classes_) {
        if (record.count != record.expected)
          determinism_failures_.push_back(
              "class size changed from " + std::to_string(record.expected) +
              " to " + std::to_string(record.count) + ": " + key);
        if (record.covering != record.expected_covering)
          determinism_failures_.push_back(
              "covering cases changed from " +
              std::to_string(record.expected_covering) + " to " +
              std::to_string(record.covering) + ": " + key);
      }
      if (observed_ != expected_observed_ || order_ != expected_order_)
        determinism_failures_.push_back(
            "the ordered case stream differs from the count pass");
      open_stream();
      Writer trailer;
      trailer.u8(2);
      trailer.u64(observed_);
      trailer.u64(written_);
      trailer.u32(static_cast<std::uint32_t>(determinism_failures_.size()));
      for (const auto &failure : determinism_failures_)
        trailer.string(failure);
      append(trailer.data());
      if (gzclose(stream_) != Z_OK)
        export_failure("cannot finish the export stream");
      stream_ = nullptr;
      std::filesystem::rename(path(".cases.gz.tmp"), path(".cases.gz"));
      std::ofstream index{path(".cases.tsv"), std::ios::trunc};
      index << "ordinal\tname\tsite\tselection\tminimization\n";
      for (const auto &line : case_index_)
        index << line << '\n';
      if (directives_) {
        for (const auto &[ordinal, directive] : forced_ordinals_)
          if (!seen_ordinals_.contains(ordinal))
            forced_report_.push_back(directive + "\t" +
                                     std::to_string(ordinal) + "\t\tunmatched");
        std::ofstream forced{path(".forced.tsv"), std::ios::trunc};
        forced << "directive\tordinal\tname\tresult\n";
        for (const auto &line : forced_report_)
          forced << line << '\n';
      }
    }
    const auto target = path(sampling_ ? ".sample-exclusions.tsv"
                                       : ".exclusions.tsv");
    std::ofstream output{target, std::ios::trunc};
    output << "reason\tsite\tcases\n";
    for (const auto &[key, count] : exclusions_)
      output << key << '\t' << count << '\n';
    if (!determinism_failures_.empty()) {
      std::fprintf(stderr, "arm64 corpus export: %zu determinism failures\n",
                   determinism_failures_.size());
      std::_Exit(3);
    }
  }

  std::string directory_;
  std::string producer_;
  pid_t creator_{};
  bool sampling_{};
  std::size_t limit_{default_sample_limit};
  std::uint64_t seed_{default_seed};
  std::unique_ptr<DecodePartitions> partitions_;
  std::mutex mutex_;
  std::map<std::string, ClassRecord> classes_;
  std::map<std::string, std::uint64_t> exclusions_;
  std::vector<std::string> determinism_failures_;
  std::uint64_t observed_{};
  std::uint64_t written_{};
  std::array<std::uint64_t, 3> minimized_{};
  gzFile stream_{};
  /* Order of (ordinal, class) observations; the sample pass must repeat the
   * count pass exactly. */
  std::uint64_t order_{};
  std::uint64_t expected_observed_{};
  std::uint64_t expected_order_{};
  /* Force-include directives that apply to this producer. */
  struct WordDirective final {
    std::uint32_t mask{};
    std::uint32_t value{};
    std::uint64_t limit{};
    std::string text;
    std::map<std::string, std::uint64_t> forced_per_class;
  };
  bool directives_{};
  std::map<std::uint64_t, std::string> forced_ordinals_;
  std::set<std::uint64_t> seen_ordinals_;
  std::vector<WordDirective> word_directives_;
  std::vector<std::string> forced_report_;
  std::vector<std::string> case_index_;
};

ExportCase::ExportCase(const ProgramCase &source)
    : name{source.name}, code{source.code.begin(), source.code.end()},
      entry_offset{source.entry_offset}, initial{source.initial},
      expected{source.expected}, event{source.event}, native{source.native},
      fault{source.fault}, compare_fault{source.compare_fault},
      syndrome{source.syndrome}, invalid_encoding{source.invalid_encoding},
      validator{source.validator},
      validator_relation_id{source.validator_relation_id},
      expected_instructions{source.expected_instructions},
      expected_instruction_pattern{source.expected_instruction_pattern} {
  memory.reserve(source.memory.size());
  for (const auto &region : source.memory)
    memory.push_back({
        .address = region.address,
        .initial = {region.initial.begin(), region.initial.end()},
        .expected = {region.expected.begin(), region.expected.end()},
        .initial_tags = {region.initial_tags.begin(),
                         region.initial_tags.end()},
        .expected_tags = {region.expected_tags.begin(),
                          region.expected_tags.end()},
        .readable = region.readable,
        .writable = region.writable,
        .executable = region.executable,
        .guest_code = region.guest_code,
        .guest_writable = region.guest_writable,
    });
  region_views_.reserve(memory.size());
  for (const auto &region : memory)
    region_views_.push_back({
        .address = region.address,
        .initial = region.initial,
        .expected = region.expected,
        .initial_tags = region.initial_tags,
        .expected_tags = region.expected_tags,
        .readable = region.readable,
        .writable = region.writable,
        .executable = region.executable,
        .guest_code = region.guest_code,
        .guest_writable = region.guest_writable,
    });
}

ProgramCase ExportCase::view() const {
  return {
      .name = name,
      .code = code,
      .entry_offset = entry_offset,
      .initial = initial,
      .expected = expected,
      .event = event,
      .native = native,
      .fault = fault,
      .compare_fault = compare_fault,
      .syndrome = syndrome,
      .invalid_encoding = invalid_encoding,
      .memory = region_views_,
      .validator = validator,
      .validator_relation_id = validator_relation_id,
      .expected_instructions = expected_instructions,
      .expected_instruction_pattern = expected_instruction_pattern,
  };
}

std::uint64_t fill_register(unsigned index) noexcept {
  return 0x5eed'0000'0000'0000ULL |
         (static_cast<std::uint64_t>(index & 31U) << 40U) |
         (0x0101'0101ULL * (static_cast<std::uint64_t>(index & 31U) + 1U));
}

std::byte fill_vector_byte(unsigned index, unsigned byte) noexcept {
  return static_cast<std::byte>(((index & 31U) * 16U + (byte & 15U)) * 0x9dU +
                                0x3bU);
}

namespace {

struct Candidates final {
  /* Index 31 is SP. */
  std::array<bool, 32> general{};
  std::array<bool, 32> vector{};
};

void add_word(Candidates &candidates, std::uint32_t word, Minimization level) {
  constexpr std::array<unsigned, 4> shifts{0U, 5U, 10U, 16U};
  const auto group = (word >> 25U) & 15U;
  const bool simd_data = (group & 7U) == 7U;
  const bool load_store = (group & 5U) == 4U;
  const bool simd_transfer = load_store && ((word >> 26U) & 1U) != 0U;
  const bool branch_system = ((word >> 26U) & 7U) == 5U;
  const bool broad = level == Minimization::broad;
  const bool vectors = simd_data || simd_transfer || broad;
  for (const auto shift : shifts) {
    const auto field = (word >> shift) & 31U;
    candidates.general[field] = true;
    if (broad && field < 30U)
      candidates.general[field + 1U] = true;
    if (vectors)
      for (unsigned offset = 0; offset < 4U; ++offset)
        candidates.vector[(field + offset) & 31U] = true;
    /* By-element forms with 16-bit elements name Vm in bits 19:16 and use
     * bit 20 as an index bit. */
    if (vectors && shift == 16U)
      candidates.vector[field & 15U] = true;
  }
  /* LD64B, ST64B, ST64BV, and ST64BV0 transfer eight consecutive registers. */
  if (broad && (word & 0xffe0'8000U) == 0xf820'8000U)
    for (unsigned offset = 0; offset < 8U; ++offset)
      if (((word & 31U) + offset) < 31U)
        candidates.general[(word & 31U) + offset] = true;
  if (branch_system || broad)
    for (const unsigned implicit : {15U, 16U, 17U, 30U, 31U})
      candidates.general[implicit] = true;
}

} // namespace

bool minimize(ExportCase &test_case, Minimization level) {
  if (level == Minimization::full)
    return false;
  Candidates candidates;
  for (const auto word : test_case.code)
    add_word(candidates, word.instruction, level);
  bool changed = false;
  for (unsigned index = 0; index < 32U; ++index) {
    if (candidates.general[index])
      continue;
    auto &initial = index == 31U ? test_case.initial.sp
                                 : test_case.initial.registers[index];
    auto &expected = index == 31U ? test_case.expected.sp
                                  : test_case.expected.registers[index];
    const auto fill = fill_register(index);
    if (initial == fill)
      continue;
    if (expected == initial)
      expected = fill;
    initial = fill;
    changed = true;
  }
  for (unsigned index = 0; index < 32U; ++index) {
    if (candidates.vector[index])
      continue;
    arm64::VectorRegister fill;
    for (unsigned byte = 0; byte < fill.bytes.size(); ++byte)
      fill.bytes[byte] = fill_vector_byte(index, byte);
    auto &initial = test_case.initial.vectors[index];
    auto &expected = test_case.expected.vectors[index];
    if (initial.bytes == fill.bytes)
      continue;
    if (expected.bytes == initial.bytes)
      expected = fill;
    initial = fill;
    changed = true;
  }
  return changed;
}

std::uint32_t option_mask(const EngineHarnessOptions &options) noexcept {
  return (options.enable_pointer_authentication ? 1U : 0U) |
         (options.enable_guarded_control_flow ? 2U : 0U) |
         (options.force_software_checksum ? 4U : 0U) |
         (options.force_software_flags ? 8U : 0U) |
         (options.force_software_sha3 ? 16U : 0U) |
         (options.force_software_fixed_transforms ? 32U : 0U) |
         (options.force_software_aes ? 64U : 0U) |
         (options.force_software_pmull ? 128U : 0U) |
         (options.force_software_dotprod ? 256U : 0U) |
         (options.force_software_i8mm ? 512U : 0U);
}

Exporter *Exporter::active() {
  static Exporter *const exporter = [] {
    const auto *directory = std::getenv("UML_ARM64_CORPUS_EXPORT");
    return directory == nullptr || directory[0] == '\0' ? nullptr
                                                        : new Exporter{};
  }();
  return exporter;
}

Exporter::Exporter() : state_{new State{}} {
  /* The exporter lives for the whole process; flush it from exit(). */
  std::atexit([] {
    if (auto *exporter = Exporter::active(); exporter != nullptr) {
      delete exporter->state_;
      exporter->state_ = nullptr;
    }
  });
}

Exporter::~Exporter() { delete state_; }

Selection Exporter::observe(const ProgramCase &test_case,
                            const EngineHarnessOptions &options,
                            const std::source_location &location,
                            std::uint64_t ordinal) {
  return state_->observe(test_case, options, location, ordinal);
}

void Exporter::exclude(const ProgramCase &test_case, std::string_view reason,
                       const std::source_location &location,
                       std::uint64_t ordinal) {
  state_->exclude(test_case, reason, location, ordinal);
}

void Exporter::write(const ExportCase &test_case, const Selection &selection,
                     const EngineHarnessOptions &options,
                     const ExportResult &result) {
  state_->write(test_case, selection, options, result);
}

} // namespace uml::emulator::tests::corpus_export
