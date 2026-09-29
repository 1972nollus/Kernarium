// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Runs the checked-in A64 instruction corpus through the production gadget
 * and JIT engines in one process. tests/unit/arm64/corpus/README.md owns the
 * pack format, the state encoding, and the named validator relations.
 */
#include "architecture_difference.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/engine_execution.hpp"
#include "cpu/arm64/gadgets/executor.hpp"
#include "cpu/engine.hpp"
#include "cpu/engine_factory.hpp"
#include "memory/static_guest_memory.hpp"
#include "platform/apple/host_features.hpp"

#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"
#include "static_mmu_test_support.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace arm64 = ::uml::cpu::arm64;
using ::uml::emulator::tests::first_architecture_difference;
using ::uml::cpu::ArmFeature;
using ::uml::cpu::ArmFeatureSet;
using ::uml::cpu::EngineKind;
using ::uml::cpu::ExitReason;
using ::uml::memory::MemoryAccess;
using ::uml::memory::MemoryFault;
using ::uml::memory::StaticGuestMemory;

constexpr std::uint64_t code_base = 0x4f00'0000;
constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr std::size_t execution_limit = 64;
constexpr unsigned long generation = 0x10000;
constexpr std::array<char, 8> pack_magic{'U', 'M', 'L', 'A',
                                         '6', '4', 'C', '\0'};
constexpr std::uint32_t pack_version = 1;
constexpr std::uint8_t profile_bit = page_size == 16384 ? 1U : 2U;
constexpr std::uint32_t unbounded = 0xffff'ffffU;
constexpr std::uint16_t no_validator = 0xffffU;
/* Option bit 1 enabled the retired FEAT_GCS guarded control flow. */
constexpr std::uint16_t retired_options = 2U;

class CorpusError final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

[[nodiscard]] std::string hex(std::uint64_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "%#" PRIx64, value);
  return text;
}

/* ------------------------------------------------------------------------ */
/* Pack decoding                                                              */

class Reader final {
public:
  explicit Reader(std::span<const std::byte> data) noexcept : data_{data} {}

  [[nodiscard]] std::span<const std::byte> bytes(std::size_t size) {
    if (size > data_.size() - offset_)
      throw CorpusError{"truncated corpus pack"};
    const auto result = data_.subspan(offset_, size);
    offset_ += size;
    return result;
  }

  [[nodiscard]] std::uint64_t little(std::size_t size) {
    const auto data = bytes(size);
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < size; ++index)
      value |= std::to_integer<std::uint64_t>(data[index]) << (index * 8U);
    return value;
  }

  [[nodiscard]] std::uint8_t u8() {
    return static_cast<std::uint8_t>(little(1));
  }
  [[nodiscard]] std::uint16_t u16() {
    return static_cast<std::uint16_t>(little(2));
  }
  [[nodiscard]] std::uint32_t u32() {
    return static_cast<std::uint32_t>(little(4));
  }
  [[nodiscard]] std::uint64_t u64() { return little(8); }

  [[nodiscard]] std::string_view string(std::size_t size) {
    const auto data = bytes(size);
    return {reinterpret_cast<const char *>(data.data()), data.size()};
  }
  [[nodiscard]] std::string_view string16() { return string(u16()); }
  [[nodiscard]] std::string_view string32() { return string(u32()); }

  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] bool done() const noexcept { return offset_ == data_.size(); }

private:
  std::span<const std::byte> data_;
  std::size_t offset_{};
};

/* Terminal events, in pack order. Code 2 was the retired FEAT_GCS fault. */
enum class Event : std::uint8_t {
  system_call,
  memory_fault,
  illegal_instruction = 3,
  breakpoint,
  floating_point_trap,
  pc_alignment,
  instruction_budget,
  fatal,
};
constexpr std::uint8_t retired_event = 2U;
constexpr std::array<std::string_view, 9> event_names{
    "system-call",        "memory-fault", "retired",
    "illegal-instruction", "breakpoint",  "floating-point-trap",
    "pc-alignment",        "instruction-budget", "fatal"};

[[nodiscard]] std::string_view event_name(Event event) noexcept {
  return event_names[static_cast<std::size_t>(event)];
}

[[nodiscard]] constexpr Event event_for(ExitReason reason) noexcept {
  switch (reason) {
  case ExitReason::system_call:
    return Event::system_call;
  case ExitReason::memory_fault:
    return Event::memory_fault;
  case ExitReason::illegal_instruction:
    return Event::illegal_instruction;
  case ExitReason::breakpoint:
    return Event::breakpoint;
  case ExitReason::floating_point_trap:
    return Event::floating_point_trap;
  case ExitReason::pc_alignment:
    return Event::pc_alignment;
  case ExitReason::instruction_budget:
  case ExitReason::interrupt:
    return Event::instruction_budget;
  case ExitReason::single_step:
  case ExitReason::fatal:
    return Event::fatal;
  }
  return Event::fatal;
}

constexpr std::array<std::string_view, 3> access_names{"read", "write",
                                                       "execute"};

struct Retirement final {
  enum class Kind : std::uint8_t { exact, pattern, unchecked };
  Kind kind{Kind::exact};
  std::uint32_t minimum{};
  std::uint32_t step{1};
  std::uint32_t maximum{unbounded};
};

struct RegionSpec final {
  std::uint64_t address{};
  std::uint32_t size{};
  std::uint8_t flags{};
  std::uint32_t blob{};
  /* Encoded (offset, length, bytes) runs that differ after execution. */
  std::span<const std::byte> runs;
  std::uint16_t run_count{};

  [[nodiscard]] bool readable() const noexcept { return (flags & 1U) != 0U; }
  [[nodiscard]] bool writable() const noexcept { return (flags & 2U) != 0U; }
  [[nodiscard]] bool executable() const noexcept { return (flags & 4U) != 0U; }
  [[nodiscard]] bool guest_code() const noexcept { return (flags & 8U) != 0U; }
  [[nodiscard]] bool guest_writable() const noexcept {
    return (flags & 16U) != 0U;
  }
};

struct CorpusCase final {
  std::uint32_t program{};
  std::string_view name;
  std::uint32_t ordinal_16k{};
  std::uint32_t ordinal_4k{};
  Event event{};
  bool invalid_encoding{};
  bool compare_fault{};
  Retirement retired;
  std::uint64_t fault_address{};
  MemoryAccess fault_access{MemoryAccess::read};
  std::uint32_t syndrome{};
  std::uint16_t native{};
  std::uint16_t validator{no_validator};
  std::span<const std::byte> initial;
  std::span<const std::byte> expected;
  std::vector<RegionSpec> memory;
};

struct ProgramWord final {
  std::uint16_t offset{};
  std::uint32_t word{};
};

struct Program final {
  std::uint16_t entry{};
  std::uint32_t entry_word{};
  std::vector<ProgramWord> words;
};

struct Record final {
  std::string_view name;
  std::string_view producer;
  std::string_view site;
  std::uint8_t profiles{};
  std::uint16_t options{};
  std::vector<Program> programs;
  std::vector<CorpusCase> cases;
};

/* Every encoded non-register state field; see the README field table. Ids
 * 7, 9-16, 20, and 32-35 held the retired FEAT_MTE, FEAT_CPA, and FEAT_GCS
 * state and stay reserved. */
enum class Field : std::uint8_t {
  pc = 1,
  nzcv,
  fpcr,
  fpsr,
  fpmr,
  tls,
  tbi_enabled = 8,
  accdata = 17,
  pstate_ssbs,
  pstate_dit,
  pstate_sm = 21,
  pstate_za,
  pstate_daif_access,
  pstate_daif,
  exclusive_address,
  exclusive_key,
  exclusive_value,
  exclusive_value2,
  exclusive_version,
  exclusive_size,
  fp_dirty,
  pointer_auth_keys = 36,
  pointer_authentication_enabled,
};

[[nodiscard]] std::size_t field_size(std::uint8_t identifier) {
  switch (static_cast<Field>(identifier)) {
  case Field::pc:
  case Field::fpmr:
  case Field::tls:
  case Field::exclusive_address:
  case Field::exclusive_key:
  case Field::exclusive_value:
  case Field::exclusive_value2:
  case Field::exclusive_version:
    return 8;
  case Field::nzcv:
  case Field::fpcr:
  case Field::fpsr:
  case Field::accdata:
    return 4;
  case Field::pointer_auth_keys:
    return 80;
  case Field::tbi_enabled:
  case Field::pstate_ssbs:
  case Field::pstate_dit:
  case Field::pstate_sm:
  case Field::pstate_za:
  case Field::pstate_daif_access:
  case Field::pstate_daif:
  case Field::exclusive_size:
  case Field::fp_dirty:
  case Field::pointer_authentication_enabled:
    return 1;
  }
  throw CorpusError{"unknown state field " + std::to_string(identifier)};
}

/* Returns the encoded span of one sparse state and validates its shape. */
[[nodiscard]] std::span<const std::byte>
sparse_state(Reader &reader, std::span<const std::byte> body) {
  const auto begin = reader.offset();
  const auto registers = reader.u32();
  (void)reader.bytes(8U * static_cast<std::size_t>(std::popcount(registers)));
  const auto vectors = reader.u32();
  (void)reader.bytes(16U * static_cast<std::size_t>(std::popcount(vectors)));
  const auto fields = reader.u8();
  for (unsigned index = 0; index < fields; ++index)
    (void)reader.bytes(field_size(reader.u8()));
  return body.subspan(begin, reader.offset() - begin);
}

[[nodiscard]] std::uint64_t fill_register(unsigned index) noexcept {
  return 0x5eed'0000'0000'0000ULL |
         (static_cast<std::uint64_t>(index & 31U) << 40U) |
         (0x0101'0101ULL * (static_cast<std::uint64_t>(index & 31U) + 1U));
}

[[nodiscard]] std::byte fill_vector_byte(unsigned index,
                                         unsigned byte) noexcept {
  return static_cast<std::byte>(((index & 31U) * 16U + (byte & 15U)) * 0x9dU +
                                0x3bU);
}

/* The base every initial state is encoded against: canonical register fill,
 * the entry PC, and the pack's default for every other exported field. */
[[nodiscard]] arm64::CpuState base_state(std::uint16_t entry) {
  arm64::CpuState state{};
  for (unsigned index = 0; index < state.registers.size(); ++index)
    state.registers[index] = fill_register(index);
  state.sp = fill_register(31);
  for (unsigned index = 0; index < state.vectors.size(); ++index)
    for (unsigned byte = 0; byte < 16U; ++byte)
      state.vectors[index].bytes[byte] = fill_vector_byte(index, byte);
  state.pc = code_base + entry;
  state.nzcv = 0;
  state.fpcr = 0;
  state.fpsr = 0;
  state.fpmr = 0;
  state.tls = 0;
  state.tbi_enabled = false;
  state.accdata = 0;
  state.pstate_ssbs = false;
  state.pstate_dit = false;
  state.pstate_sm = false;
  state.pstate_za = false;
  state.pstate_daif_access = false;
  state.pstate_daif = 0;
  state.exclusive_address = ~std::uint64_t{0};
  state.exclusive_key = 0;
  state.exclusive_value = 0;
  state.exclusive_value2 = 0;
  state.exclusive_version = 0;
  state.exclusive_size = 0;
  state.fp_dirty = false;
  state.pointer_auth_keys = arm64::deterministic_pointer_auth_keys;
  state.pointer_authentication_enabled = true;
  return state;
}

void apply_sparse(arm64::CpuState &state, std::span<const std::byte> encoded) {
  Reader reader{encoded};
  const auto registers = reader.u32();
  for (unsigned index = 0; index < 32U; ++index)
    if ((registers >> index) & 1U) {
      const auto value = reader.u64();
      if (index == 31U)
        state.sp = value;
      else
        state.registers[index] = value;
    }
  const auto vectors = reader.u32();
  for (unsigned index = 0; index < 32U; ++index)
    if ((vectors >> index) & 1U) {
      const auto data = reader.bytes(16);
      std::copy(data.begin(), data.end(), state.vectors[index].bytes.begin());
    }
  const auto fields = reader.u8();
  for (unsigned field = 0; field < fields; ++field) {
    const auto identifier = reader.u8();
    const auto flag = [&] { return reader.u8() != 0U; };
    switch (static_cast<Field>(identifier)) {
    case Field::pc:
      state.pc = reader.u64();
      break;
    case Field::nzcv:
      state.nzcv = reader.u32();
      break;
    case Field::fpcr:
      state.fpcr = reader.u32();
      break;
    case Field::fpsr:
      state.fpsr = reader.u32();
      break;
    case Field::fpmr:
      state.fpmr = reader.u64();
      break;
    case Field::tls:
      state.tls = reader.u64();
      break;
    case Field::tbi_enabled:
      state.tbi_enabled = flag();
      break;
    case Field::accdata:
      state.accdata = reader.u32();
      break;
    case Field::pstate_ssbs:
      state.pstate_ssbs = flag();
      break;
    case Field::pstate_dit:
      state.pstate_dit = flag();
      break;
    case Field::pstate_sm:
      state.pstate_sm = flag();
      break;
    case Field::pstate_za:
      state.pstate_za = flag();
      break;
    case Field::pstate_daif_access:
      state.pstate_daif_access = flag();
      break;
    case Field::pstate_daif:
      state.pstate_daif = reader.u8();
      break;
    case Field::exclusive_address:
      state.exclusive_address = reader.u64();
      break;
    case Field::exclusive_key:
      state.exclusive_key = static_cast<std::uintptr_t>(reader.u64());
      break;
    case Field::exclusive_value:
      state.exclusive_value = reader.u64();
      break;
    case Field::exclusive_value2:
      state.exclusive_value2 = reader.u64();
      break;
    case Field::exclusive_version:
      state.exclusive_version = reader.u64();
      break;
    case Field::exclusive_size:
      state.exclusive_size = reader.u8();
      break;
    case Field::fp_dirty:
      state.fp_dirty = flag();
      break;
    case Field::pointer_auth_keys:
      for (auto &key : state.pointer_auth_keys) {
        key.low = reader.u64();
        key.high = reader.u64();
      }
      break;
    case Field::pointer_authentication_enabled:
      state.pointer_authentication_enabled = flag();
      break;
    default:
      throw CorpusError{"unknown state field " + std::to_string(identifier)};
    }
  }
  if (!reader.done())
    throw CorpusError{"trailing sparse state data"};
}

/* ------------------------------------------------------------------------ */
/* Architectural comparison and validator relations                         */

struct RelationInput final {
  /* Initial and expected states are normalized exactly like the actual one. */
  const arm64::CpuState &initial;
  const arm64::CpuState &expected;
  const arm64::CpuState &actual;
  const Program &program;
};

using Relation = std::optional<std::string> (*)(const RelationInput &);

[[nodiscard]] constexpr std::uint64_t strip(std::uint64_t source,
                                            bool data) noexcept {
  const bool upper = (source & (std::uint64_t{1} << 55U)) != 0U;
  if (data && !upper)
    return source & 0xff00'7fff'ffff'ffffULL;
  return (source & 0x0000'7fff'ffff'ffffULL) |
         (upper ? 0xffff'8000'0000'0000ULL : 0U);
}

[[nodiscard]] constexpr std::uint64_t signing_original(std::uint64_t source,
                                                       bool data) noexcept {
  const bool upper = (source & (std::uint64_t{1} << 55U)) != 0U;
  if (data && !upper)
    return source & 0xff00'7fff'ffff'ffffULL;
  return (source & 0x0000'7fff'ffff'ffffULL) |
         ((source >> 63U) != 0U ? 0xffff'8000'0000'0000ULL : 0U);
}

[[nodiscard]] std::optional<std::uint32_t> word_at(const Program &program,
                                                   std::size_t offset) {
  for (const auto word : program.words)
    if (word.offset == offset)
      return word.word;
  return std::nullopt;
}

/* A failed store-exclusive clears the monitor; the retained monitor payload
 * is opaque, so only its identity and size are compared. */
std::optional<std::string> exclusive_store_failure(const RelationInput &in) {
  auto normalized = in.actual;
  normalized.exclusive_value = in.expected.exclusive_value;
  normalized.exclusive_value2 = in.expected.exclusive_value2;
  normalized.exclusive_version = in.expected.exclusive_version;
  return first_architecture_difference(in.expected, normalized);
}

/* PAC*, AUT*, and their HINT aliases: the retained pointer bits follow the
 * architectural relation while the inserted code is implementation-defined. */
std::optional<std::string> transformed_code(const RelationInput &in) {
  const auto word = in.program.entry_word;
  unsigned destination = word & 31U;
  bool data = false;
  bool authenticate = false;
  if ((word & 0xffff'c000U) == 0xdac1'0000U) {
    data = ((word >> 10U) & 2U) != 0U;
    authenticate = ((word >> 10U) & 4U) != 0U;
  } else {
    const unsigned immediate = (word >> 5U) & 0x7fU;
    destination = immediate < 16U ? 17U : 30U;
    authenticate = immediate == 12U || immediate == 14U ||
                   (immediate >= 28U && immediate <= 31U);
  }
  auto expected = in.initial;
  expected.pc = in.expected.pc;
  if (destination < 31U) {
    const auto before = in.initial.registers[destination];
    const auto after = in.actual.registers[destination];
    const auto retained =
        authenticate ? strip(before, data) : signing_original(before, data);
    if (strip(after, data) != retained)
      return "x" + std::to_string(destination) + " " + hex(after) +
             " does not retain " + hex(before);
    expected.registers[destination] = after;
  }
  return first_architecture_difference(expected, in.actual);
}

/* PACGA: the low half of the generic code is zero. */
std::optional<std::string> transformed_generic(const RelationInput &in) {
  const unsigned destination = in.program.entry_word & 31U;
  auto expected = in.initial;
  expected.pc = in.expected.pc;
  if (destination < 31U) {
    if ((in.actual.registers[destination] & 0xffff'ffffULL) != 0U)
      return "generic code " + hex(in.actual.registers[destination]) +
             " has a non-zero low half";
    expected.registers[destination] = in.actual.registers[destination];
  }
  return first_architecture_difference(expected, in.actual);
}

/* A failed authenticated branch or return faults at a transformed target.
 * Programs that sign the target register with itself as the modifier and
 * then branch fail with the original address in the retained bits. */
std::optional<std::string> transformed_branch_failure(const RelationInput &in) {
  const auto word = in.program.entry_word;
  auto expected = in.initial;
  expected.pc = in.actual.pc;
  if ((word & 0xffff'c000U) == 0xdac1'0000U) {
    const auto branch = word_at(in.program, in.program.entry + 4U);
    if (!branch.has_value())
      return std::string{"relation requires a branch after the signing entry"};
    const bool link = (*branch & 0x0020'0000U) != 0U;
    const unsigned target = word & 31U;
    if (target == 31U)
      return std::string{"relation requires a general target register"};
    if (strip(in.actual.pc, false) != in.initial.registers[target])
      return "failed target " + hex(in.actual.pc) + " does not retain " +
             hex(in.initial.registers[target]);
    if (!link || target != 30U) {
      if (strip(in.actual.registers[target], false) !=
          in.initial.registers[target])
        return "x" + std::to_string(target) + " " +
               hex(in.actual.registers[target]) + " does not retain " +
               hex(in.initial.registers[target]);
      expected.registers[target] = in.actual.registers[target];
    }
    if (link)
      expected.registers[30] = code_base + in.program.entry + 8U;
  } else {
    if (strip(in.actual.pc, false) != 0U)
      return "failed target " + hex(in.actual.pc) + " is not a zero address";
    if ((word & 0x0020'0000U) != 0U)
      expected.registers[30] = code_base + in.program.entry + 4U;
  }
  return first_architecture_difference(expected, in.actual);
}

/* An authenticated load through a signed base faults with only that base
 * transformed. */
std::optional<std::string> transformed_address_fault(const RelationInput &in) {
  const unsigned base = in.program.entry_word & 31U;
  if (base == 31U)
    return std::string{"relation requires a general base register"};
  if (strip(in.actual.registers[base], true) != in.initial.registers[base])
    return "x" + std::to_string(base) + " " + hex(in.actual.registers[base]) +
           " does not retain " + hex(in.initial.registers[base]);
  auto expected = in.initial;
  expected.pc = in.expected.pc;
  expected.registers[base] = in.actual.registers[base];
  return first_architecture_difference(expected, in.actual);
}

/* A system-register read of the host counter is excluded from equality. */
std::optional<std::string> ignore_counter_register(const RelationInput &in) {
  auto expected = in.expected;
  for (const auto word : in.program.words) {
    const unsigned destination = word.word & 31U;
    if ((word.word & 0xfff0'0000U) == 0xd530'0000U && destination != 31U)
      expected.registers[destination] = in.actual.registers[destination];
  }
  return first_architecture_difference(expected, in.actual);
}

[[nodiscard]] Relation relation_for(std::string_view name) {
  static const std::map<std::string_view, Relation> relations{
      {"ignore-host-counter-register/v1", ignore_counter_register},
      {"permitted-exclusive-store-failure/v1", exclusive_store_failure},
      {"permitted-transformed-branch-failure/v1", transformed_branch_failure},
      {"permitted-transformed-code/v1", transformed_code},
      {"permitted-transformed-generic/v1", transformed_generic},
      {"transformed-address-fault-state/v1", transformed_address_fault},
  };
  if (const auto found = relations.find(name); found != relations.end())
    return found->second;
  throw CorpusError{"the runner does not implement validator relation " +
                    std::string{name}};
}

/* ------------------------------------------------------------------------ */
/* Corpus                                                                     */

struct Corpus final {
  std::vector<std::byte> body;
  std::string meta;
  std::vector<ArmFeatureSet> native_sets;
  std::vector<std::string_view> tokens;
  std::vector<std::string_view> validator_names;
  std::vector<Relation> relations;
  std::vector<std::span<const std::byte>> blobs;
  std::vector<Record> records;
};

[[nodiscard]] std::vector<std::byte> read_file(const char *path) {
  std::ifstream input{path, std::ios::binary};
  if (!input)
    throw CorpusError{std::string{"cannot open "} + path};
  input.seekg(0, std::ios::end);
  const auto size = static_cast<std::size_t>(input.tellg());
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> data(size);
  if (!input.read(reinterpret_cast<char *>(data.data()),
                  static_cast<std::streamsize>(size)))
    throw CorpusError{std::string{"cannot read "} + path};
  return data;
}

[[nodiscard]] std::vector<std::byte> inflate_pack(const char *path) {
  const auto file = read_file(path);
  Reader header{file};
  const auto magic = header.bytes(pack_magic.size());
  if (!std::equal(magic.begin(), magic.end(), pack_magic.begin(),
                  [](std::byte left, char right) {
                    return std::to_integer<char>(left) == right;
                  }))
    throw CorpusError{"not an A64 instruction corpus pack"};
  if (header.u32() != pack_version || header.u32() != 0U)
    throw CorpusError{"unsupported corpus pack version"};
  const auto size = header.u64();
  std::vector<std::byte> body(static_cast<std::size_t>(size));
  auto length = static_cast<uLongf>(size);
  const auto compressed = std::span{file}.subspan(header.offset());
  if (uncompress(reinterpret_cast<Bytef *>(body.data()), &length,
                 reinterpret_cast<const Bytef *>(compressed.data()),
                 static_cast<uLong>(compressed.size())) != Z_OK ||
      length != size)
    throw CorpusError{"corrupt corpus pack body"};
  return body;
}

[[nodiscard]] ArmFeatureSet
feature_set(std::uint16_t index, const std::vector<std::string_view> &tokens) {
  if (index >= tokens.size())
    throw CorpusError{"native feature index out of range"};
  const auto &known = ::uml::platform::arm_feature_tokens;
  const auto found = std::find(known.begin(), known.end(), tokens[index]);
  if (found == known.end())
    throw CorpusError{"the runtime does not define feature " +
                      std::string{tokens[index]}};
  ArmFeatureSet result;
  result.set(static_cast<std::size_t>(found - known.begin()));
  return result;
}

void validate_case(const Record &record, const CorpusCase &test_case,
                   const Corpus &corpus) {
  const auto fail = [&](std::string_view message) {
    throw CorpusError{std::string{record.name} + ": " +
                      std::string{test_case.name} + ": " +
                      std::string{message}};
  };
  if (test_case.program >= record.programs.size())
    fail("program index out of range");
  for (const auto word : record.programs[test_case.program].words)
    if (word.offset > page_size - 4U)
      fail("program word lies outside the code page");
  if (test_case.native >= corpus.native_sets.size())
    fail("native set index out of range");
  if (test_case.validator != no_validator &&
      test_case.validator >= corpus.relations.size())
    fail("validator index out of range");
  if (test_case.retired.kind == Retirement::Kind::pattern &&
      (test_case.retired.step == 0U ||
       test_case.retired.maximum < test_case.retired.minimum))
    fail("invalid retirement pattern");
  for (std::size_t index = 0; index < test_case.memory.size(); ++index) {
    const auto &region = test_case.memory[index];
    if (region.blob >= corpus.blobs.size() ||
        corpus.blobs[region.blob].size() != region.size)
      fail("memory blob does not match its region");
    if (region.size == 0U || region.size % page_size != 0U ||
        region.address % page_size != 0U ||
        region.address > ~std::uint64_t{0} - region.size)
      fail("invalid memory-region shape");
    if (region.address < code_base + page_size &&
        code_base < region.address + region.size)
      fail("memory region overlaps code");
    for (std::size_t prior = 0; prior < index; ++prior) {
      const auto &other = test_case.memory[prior];
      if (region.address < other.address + other.size &&
          other.address < region.address + region.size)
        fail("memory regions overlap");
    }
  }
}

[[nodiscard]] Corpus load_corpus(const char *path) {
  Corpus corpus;
  corpus.body = inflate_pack(path);
  const std::span<const std::byte> body{corpus.body};
  Reader reader{body};
  corpus.meta = std::string{reader.string32()};
  const auto token_count = reader.u16();
  for (unsigned index = 0; index < token_count; ++index)
    corpus.tokens.push_back(reader.string16());
  const auto native_count = reader.u16();
  for (unsigned index = 0; index < native_count; ++index) {
    ArmFeatureSet set;
    const auto features = reader.u8();
    for (unsigned feature = 0; feature < features; ++feature)
      set |= feature_set(reader.u16(), corpus.tokens);
    corpus.native_sets.push_back(set);
  }
  const auto validator_count = reader.u16();
  for (unsigned index = 0; index < validator_count; ++index) {
    corpus.validator_names.push_back(reader.string16());
    corpus.relations.push_back(relation_for(corpus.validator_names.back()));
  }
  const auto blob_count = reader.u32();
  for (std::uint32_t index = 0; index < blob_count; ++index)
    corpus.blobs.push_back(reader.bytes(reader.u32()));
  const auto record_count = reader.u32();
  corpus.records.reserve(record_count);
  for (std::uint32_t index = 0; index < record_count; ++index) {
    auto &record = corpus.records.emplace_back();
    record.name = reader.string16();
    record.producer = reader.string16();
    record.site = reader.string16();
    record.profiles = reader.u8();
    record.options = reader.u16();
    if ((record.options & retired_options) != 0U)
      throw CorpusError{std::string{record.name} + ": retired harness option"};
    const auto program_count = reader.u32();
    record.programs.reserve(program_count);
    for (std::uint32_t program = 0; program < program_count; ++program) {
      auto &entry = record.programs.emplace_back();
      entry.entry = reader.u16();
      const auto words = reader.u16();
      bool found_entry = false;
      for (unsigned word = 0; word < words; ++word) {
        const ProgramWord item{.offset = reader.u16(), .word = reader.u32()};
        if (item.offset % 4U != 0U)
          throw CorpusError{std::string{record.name} + ": invalid program"};
        for (const auto prior : entry.words)
          if (prior.offset == item.offset)
            throw CorpusError{std::string{record.name} +
                              ": duplicate program word"};
        if (item.offset == entry.entry) {
          entry.entry_word = item.word;
          found_entry = true;
        }
        entry.words.push_back(item);
      }
      if (!found_entry)
        throw CorpusError{std::string{record.name} +
                          ": program entry word is absent"};
    }
    const auto case_count = reader.u32();
    record.cases.reserve(case_count);
    for (std::uint32_t item = 0; item < case_count; ++item) {
      auto &test_case = record.cases.emplace_back();
      test_case.program = reader.u32();
      test_case.name = reader.string16();
      test_case.ordinal_16k = reader.u32();
      test_case.ordinal_4k = reader.u32();
      const auto event = reader.u8();
      if (event >= event_names.size() || event == retired_event)
        throw CorpusError{std::string{record.name} + ": unknown event"};
      test_case.event = static_cast<Event>(event);
      const auto flags = reader.u8();
      test_case.invalid_encoding = (flags & 1U) != 0U;
      test_case.compare_fault = (flags & 2U) != 0U;
      switch (reader.u8()) {
      case 0:
        test_case.retired = {.kind = Retirement::Kind::exact,
                             .minimum = reader.u32()};
        break;
      case 1: {
        const auto minimum = reader.u32();
        const auto step = reader.u32();
        const auto maximum = reader.u32();
        test_case.retired = {.kind = Retirement::Kind::pattern,
                             .minimum = minimum,
                             .step = step,
                             .maximum = maximum};
        break;
      }
      case 2:
        test_case.retired = {.kind = Retirement::Kind::unchecked};
        break;
      default:
        throw CorpusError{std::string{record.name} +
                          ": unknown retirement kind"};
      }
      if (test_case.compare_fault) {
        test_case.fault_address = reader.u64();
        const auto access = reader.u8();
        if (access >= access_names.size())
          throw CorpusError{std::string{record.name} + ": unknown access"};
        test_case.fault_access = static_cast<MemoryAccess>(access);
      }
      if (test_case.event == Event::floating_point_trap)
        test_case.syndrome = reader.u32();
      test_case.native = reader.u16();
      test_case.validator = reader.u16();
      test_case.initial = sparse_state(reader, body);
      test_case.expected = sparse_state(reader, body);
      const auto regions = reader.u8();
      for (unsigned region_index = 0; region_index < regions; ++region_index) {
        auto &region = test_case.memory.emplace_back();
        region.address = reader.u64();
        region.size = reader.u32();
        region.flags = reader.u8();
        region.blob = reader.u32();
        region.run_count = reader.u16();
        const auto runs_begin = reader.offset();
        for (unsigned run = 0; run < region.run_count; ++run) {
          const auto offset = reader.u32();
          const auto length = reader.u16();
          if (offset > region.size || length > region.size - offset)
            throw CorpusError{std::string{record.name} +
                              ": memory run out of range"};
          (void)reader.bytes(length);
        }
        region.runs = body.subspan(runs_begin, reader.offset() - runs_begin);
        /* The two reserved allocation-tag list counts. */
        if (reader.u16() != 0U || reader.u16() != 0U)
          throw CorpusError{std::string{record.name} +
                            ": retired allocation tags"};
      }
      /* Region shapes depend on the page profile a record targets. */
      if ((record.profiles & profile_bit) != 0U)
        validate_case(record, test_case, corpus);
    }
  }
  if (!reader.done())
    throw CorpusError{"trailing corpus pack data"};
  return corpus;
}

/* ------------------------------------------------------------------------ */
/* Guest mapping                                                              */

[[noreturn]] void host_failure(std::string_view what) {
  throw std::runtime_error{std::string{what} + ": " + std::strerror(errno)};
}

/* An unnamed POSIX shared-memory object with a host view. */
class SharedBacking final {
public:
  explicit SharedBacking(std::size_t size) : size_{size} {
    static std::uint64_t serial = 0;
    char name[32];
    std::snprintf(name, sizeof(name), "/a64c.%d.%" PRIu64,
                  static_cast<int>(::getpid()), ++serial);
    descriptor_ = ::shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (descriptor_ < 0)
      host_failure("shm_open");
    (void)::shm_unlink(name);
    if (::ftruncate(descriptor_, static_cast<off_t>(size)) != 0)
      host_failure("ftruncate");
    void *view = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        descriptor_, 0);
    if (view == MAP_FAILED)
      host_failure("mmap");
    view_ = static_cast<std::byte *>(view);
  }

  ~SharedBacking() {
    if (view_ != nullptr)
      (void)::munmap(view_, size_);
    if (descriptor_ >= 0)
      (void)::close(descriptor_);
  }

  SharedBacking(const SharedBacking &) = delete;
  SharedBacking &operator=(const SharedBacking &) = delete;

  [[nodiscard]] int descriptor() const noexcept { return descriptor_; }
  [[nodiscard]] std::span<std::byte> view() const noexcept {
    return {view_, size_};
  }

private:
  std::size_t size_;
  int descriptor_{-1};
  std::byte *view_{};
};

class AddressSpace final {
public:
  AddressSpace() {
    if (darwin_uml_static_vm_activate(generation) != 0)
      throw std::runtime_error{"cannot activate the guest address space"};
  }
  ~AddressSpace() { (void)darwin_uml_static_vm_destroy(generation); }

  AddressSpace(const AddressSpace &) = delete;
  AddressSpace &operator=(const AddressSpace &) = delete;
};

class GuestMapping final {
public:
  GuestMapping(std::uint64_t address, const SharedBacking &backing,
               std::size_t size, int protection, bool guest_code,
               bool guest_writable)
      : address_{address}, size_{size} {
    if (darwin_uml_static_vm_map_generation(
            generation, reinterpret_cast<void *>(address), backing.descriptor(),
            0, size, protection, guest_code ? 1 : 0,
            guest_writable ? 1 : 0) != 0)
      throw std::runtime_error{"cannot map guest region " + hex(address)};
  }

  ~GuestMapping() {
    (void)darwin_uml_static_vm_unmap_generation(
        generation, reinterpret_cast<void *>(address_), size_);
  }

  GuestMapping(const GuestMapping &) = delete;
  GuestMapping &operator=(const GuestMapping &) = delete;

private:
  std::uint64_t address_;
  std::size_t size_;
};

/* One declared data region, mapped for the lifetime of one case. */
class CaseRegion final {
public:
  CaseRegion(const RegionSpec &spec, std::span<const std::byte> initial)
      : spec_{spec}, initial_{initial}, backing_{spec.size},
        mapping_{spec.address,
                 backing_,
                 spec.size,
                 (spec.readable() ? PROT_READ : 0) |
                     (spec.writable() ? PROT_WRITE : 0) |
                     (spec.executable() ? PROT_EXEC : 0),
                 spec.guest_code(),
                 spec.guest_writable()} {
    if (spec.run_count != 0U) {
      expected_.assign(initial.begin(), initial.end());
      Reader runs{spec.runs};
      for (unsigned run = 0; run < spec.run_count; ++run) {
        const auto offset = runs.u32();
        const auto data = runs.bytes(runs.u16());
        std::copy(data.begin(), data.end(), expected_.begin() + offset);
      }
    }
  }

  CaseRegion(const CaseRegion &) = delete;
  CaseRegion &operator=(const CaseRegion &) = delete;

  void restore() const {
    std::copy(initial_.begin(), initial_.end(), backing_.view().begin());
  }

  [[nodiscard]] std::optional<std::string> check() const {
    const auto actual = backing_.view();
    const std::span<const std::byte> expected =
        expected_.empty() ? initial_ : std::span<const std::byte>{expected_};
    const auto mismatch =
        std::mismatch(actual.begin(), actual.end(), expected.begin());
    if (mismatch.first != actual.end()) {
      const auto offset =
          static_cast<std::uint64_t>(mismatch.first - actual.begin());
      return "memory at " + hex(spec_.address + offset) + " expected " +
             hex(std::to_integer<unsigned>(*mismatch.second)) + " actual " +
             hex(std::to_integer<unsigned>(*mismatch.first));
    }
    return std::nullopt;
  }

private:
  const RegionSpec &spec_;
  std::span<const std::byte> initial_;
  SharedBacking backing_;
  GuestMapping mapping_;
  std::vector<std::byte> expected_;
};

/* ------------------------------------------------------------------------ */
/* Engines                                                                    */

enum class Lane : std::uint8_t { gadgets, jit };

[[nodiscard]] std::string_view lane_name(Lane lane) noexcept {
  return lane == Lane::gadgets ? "gadgets" : "jit";
}

struct Outcome final {
  Event event{Event::fatal};
  arm64::CpuState cpu;
  std::size_t instructions{};
  MemoryFault fault{};
  std::uint32_t syndrome{};
};

/* The production engines for one harness option set. Options that force a
 * portable gadget body select the gadget executor directly, as the harness
 * that produced those cases did. */
class Engines final {
public:
  explicit Engines(std::uint16_t options) {
    const bool pointer_authentication = (options & 1U) != 0U;
    const auto &host = ::uml::platform::apple::probed_host_capabilities();
    const ::uml::cpu::EngineConfiguration configuration{
        .enable_pointer_authentication = pointer_authentication,
        .host_capabilities = host,
    };
    if ((options & 0x3fcU) == 0U) {
      gadgets_ =
          ::uml::cpu::make_cpu_engine(EngineKind::gadgets, configuration);
    } else {
      forced_ = std::make_unique<arm64::gadgets::GadgetExecutor>(
          arm64::gadgets::BuildOptions{
              .enable_pointer_authentication = pointer_authentication,
              .force_software_checksum = (options & 4U) != 0U,
              .force_software_flags = (options & 8U) != 0U,
              .force_software_sha3 = (options & 16U) != 0U,
              .force_software_fixed_transforms = (options & 32U) != 0U,
              .force_software_aes = (options & 64U) != 0U,
              .force_software_pmull = (options & 128U) != 0U,
              .force_software_dotprod = (options & 256U) != 0U,
              .force_software_i8mm = (options & 512U) != 0U,
              .host_capabilities = host,
          });
    }
    if (::uml::cpu::cpu_engine_available(EngineKind::jit))
      jit_ = ::uml::cpu::make_cpu_engine(EngineKind::jit, configuration);
  }

  [[nodiscard]] bool jit_available() const noexcept { return jit_ != nullptr; }

  [[nodiscard]] Outcome run(Lane lane, const arm64::CpuState &initial,
                            StaticGuestMemory &memory) {
    Outcome outcome;
    outcome.cpu = initial;
    const auto sequences = darwin_uml_static_vm_sequences(generation);
    memory.prepare(generation, sequences.mapping_changes);
    ::uml::cpu::ExecutionRequest request{
        .cpu = outcome.cpu,
        .memory = memory,
        .translated_memory = &memory,
        .generation = generation,
        .instruction_budget = execution_limit,
    };
    ::uml::cpu::ExecutionResult result;
    if (lane == Lane::gadgets && forced_ != nullptr)
      result = arm64::detail::run_translated(*forced_, request);
    else
      result = (lane == Lane::gadgets ? gadgets_ : jit_)->run(request);
    outcome.event = event_for(result.reason);
    outcome.instructions = result.retired_instructions;
    outcome.fault = {result.fault.address, result.fault.access};
    outcome.syndrome = result.syndrome;
    return outcome;
  }

private:
  std::unique_ptr<::uml::cpu::CpuEngine> gadgets_;
  std::unique_ptr<arm64::gadgets::GadgetExecutor> forced_;
  std::unique_ptr<::uml::cpu::CpuEngine> jit_;
};

/* ------------------------------------------------------------------------ */
/* Runner                                                                     */

struct Options final {
  const char *pack{};
  std::vector<std::string_view> records;
  std::vector<std::string_view> producers;
  std::vector<std::string_view> cases;
  std::vector<std::uint32_t> ordinals;
  bool run_gadgets{true};
  bool run_jit{true};
  std::size_t max_failures{25};
  bool list{};
};

struct Counts final {
  std::size_t records{};
  std::size_t cases{};
  std::size_t gadgets{};
  std::size_t jit{};
  std::size_t jit_skipped_host_feature{};
  std::size_t gadgets_skipped_host_feature{};
  std::size_t jit_unavailable{};
  std::size_t failures{};
  std::map<std::string_view, std::size_t> missing_features;
};

class Runner final {
public:
  Runner(const Corpus &corpus, const Options &options)
      : corpus_{corpus}, options_{options},
        host_{::uml::platform::apple::probed_host_capabilities()},
        code_backing_{page_size},
        code_mapping_{code_base, code_backing_, page_size,
                      PROT_READ | PROT_EXEC, true, false} {
    if (!::uml::cpu::cpu_engine_available(EngineKind::gadgets))
      throw std::runtime_error{"the gadget engine is unavailable"};
  }

  /* Returns false once the failure limit stops the run. */
  bool run(const std::vector<const Record *> &records) {
    std::optional<std::uint16_t> engine_options;
    std::unique_ptr<Engines> engines;
    for (const auto *record : records) {
      if (!engine_options.has_value() || *engine_options != record->options) {
        engines.reset();
        engines = std::make_unique<Engines>(record->options);
        engine_options = record->options;
      }
      if (!run_record(*record, *engines))
        return false;
    }
    return true;
  }

  [[nodiscard]] const Counts &counts() const noexcept { return counts_; }

private:
  [[nodiscard]] bool selected(const CorpusCase &test_case) const {
    const auto ordinal =
        profile_bit == 1U ? test_case.ordinal_16k : test_case.ordinal_4k;
    if (!options_.ordinals.empty() &&
        std::ranges::find(options_.ordinals, ordinal) ==
            options_.ordinals.end())
      return false;
    if (options_.cases.empty())
      return true;
    return std::ranges::any_of(options_.cases, [&](std::string_view text) {
      return test_case.name.find(text) != std::string_view::npos;
    });
  }

  bool run_record(const Record &record, Engines &engines) {
    std::vector<std::vector<std::size_t>> by_program(record.programs.size());
    bool any = false;
    for (std::size_t index = 0; index < record.cases.size(); ++index)
      if (selected(record.cases[index])) {
        by_program[record.cases[index].program].push_back(index);
        any = true;
      }
    if (any)
      ++counts_.records;
    for (std::size_t program = 0; program < by_program.size(); ++program) {
      if (by_program[program].empty())
        continue;
      install(record.programs[program]);
      for (const auto index : by_program[program])
        if (!run_case(record, index, engines))
          return false;
    }
    return true;
  }

  void install(const Program &program) {
    page_.fill(std::byte{});
    for (const auto word : program.words)
      for (unsigned byte = 0; byte < 4U; ++byte)
        page_[word.offset + byte] =
            static_cast<std::byte>((word.word >> (byte * 8U)) & 0xffU);
    std::size_t first = 0;
    while (first != page_.size() && page_[first] == installed_[first])
      ++first;
    if (first == page_.size())
      return;
    std::size_t end = page_.size();
    while (end != first && page_[end - 1U] == installed_[end - 1U])
      --end;
    if (darwin_uml_static_vm_copy_from_buffer(code_base + first,
                                              page_.data() + first,
                                              end - first) != 0)
      throw std::runtime_error{"cannot install a corpus program"};
    installed_ = page_;
  }

  bool run_case(const Record &record, std::size_t index, Engines &engines) {
    const auto &test_case = record.cases[index];
    const auto &program = record.programs[test_case.program];
    ++counts_.cases;
    auto initial = base_state(program.entry);
    apply_sparse(initial, test_case.initial);
    auto expected = initial;
    apply_sparse(expected, test_case.expected);

    std::vector<std::unique_ptr<CaseRegion>> regions;
    regions.reserve(test_case.memory.size());
    for (const auto &region : test_case.memory)
      regions.push_back(std::make_unique<CaseRegion>(
          region, corpus_.blobs[region.blob]));

    for (const auto lane : {Lane::gadgets, Lane::jit}) {
      if (lane == Lane::gadgets) {
        if (!options_.run_gadgets)
          continue;
        /* The gadget engine emulates every other feature, but it completes
         * a misaligned single-copy-atomic access with the host's own
         * FEAT_LSE2, so a case that needs LSE2 has no gadget result on a
         * host without it. */
        if (corpus_.native_sets[test_case.native].test(
                static_cast<std::size_t>(ArmFeature::lse2)) &&
            !host_.has(ArmFeature::lse2)) {
          ++counts_.gadgets_skipped_host_feature;
          continue;
        }
      }
      if (lane == Lane::jit) {
        if (!options_.run_jit)
          continue;
        if (!engines.jit_available()) {
          ++counts_.jit_unavailable;
          continue;
        }
        const auto &native = corpus_.native_sets[test_case.native];
        if (!test_case.invalid_encoding && !host_.supports(native)) {
          ++counts_.jit_skipped_host_feature;
          const auto missing = native & ~host_.supported;
          for (std::size_t feature = 0; feature < missing.size(); ++feature)
            if (missing.test(feature))
              ++counts_.missing_features
                    [::uml::platform::arm_feature_tokens[feature]];
          continue;
        }
      }
      for (const auto &region : regions)
        region->restore();
      const auto outcome = engines.run(lane, initial, memory_);
      ++(lane == Lane::gadgets ? counts_.gadgets : counts_.jit);
      if (auto failure = check(test_case, program, initial, expected, outcome,
                               regions)) {
        report(record, index, lane, *failure);
        if (++counts_.failures == options_.max_failures)
          return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::optional<std::string>
  check(const CorpusCase &test_case, const Program &program,
        const arm64::CpuState &initial, const arm64::CpuState &expected,
        const Outcome &outcome,
        const std::vector<std::unique_ptr<CaseRegion>> &regions) const {
    if (outcome.cpu.gadget_fp_active || outcome.cpu.gadget_fpmr_active ||
        outcome.cpu.gadget_indirect_miss != nullptr)
      return std::string{
          "engine-private execution state escaped the public boundary"};
    if (outcome.event != test_case.event)
      return "event expected " + std::string{event_name(test_case.event)} +
             " actual " + std::string{event_name(outcome.event)} +
             " (instructions " + std::to_string(outcome.instructions) +
             ", fault " + hex(outcome.fault.address) + ")";
    const auto retired = outcome.instructions;
    switch (test_case.retired.kind) {
    case Retirement::Kind::exact:
      if (retired != test_case.retired.minimum)
        return "retired " + std::to_string(retired) +
               " instructions, expected " +
               std::to_string(test_case.retired.minimum);
      break;
    case Retirement::Kind::pattern: {
      const auto &pattern = test_case.retired;
      if (retired < pattern.minimum ||
          (pattern.maximum != unbounded && retired > pattern.maximum) ||
          (retired - pattern.minimum) % pattern.step != 0U)
        return "retired " + std::to_string(retired) +
               " instructions, expected " + std::to_string(pattern.minimum) +
               "+N*" + std::to_string(pattern.step);
      break;
    }
    case Retirement::Kind::unchecked:
      break;
    }
    /* Deferred state is part of the engine-entry contract; normalize only
     * copies after execution, exactly as the producing harness did. */
    auto compared_initial = initial;
    auto compared_expected = expected;
    auto compared_actual = outcome.cpu;
    compared_initial.access_fp_state();
    compared_expected.access_fp_state();
    compared_actual.access_fp_state();
    const auto state_failure =
        test_case.validator == no_validator
            ? first_architecture_difference(compared_expected, compared_actual)
            : corpus_.relations[test_case.validator](
                  {.initial = compared_initial,
                   .expected = compared_expected,
                   .actual = compared_actual,
                   .program = program});
    if (state_failure.has_value())
      return test_case.validator == no_validator
                 ? *state_failure
                 : std::string{corpus_.validator_names[test_case.validator]} +
                       ": " + *state_failure;
    if (test_case.compare_fault &&
        (outcome.fault.address != test_case.fault_address ||
         outcome.fault.access != test_case.fault_access))
      return "fault expected " +
             std::string{access_names[static_cast<std::size_t>(
                 test_case.fault_access)]} +
             " " + hex(test_case.fault_address) + " actual " +
             std::string{access_names[static_cast<std::size_t>(
                 outcome.fault.access)]} +
             " " + hex(outcome.fault.address);
    if (test_case.event == Event::floating_point_trap &&
        outcome.syndrome != test_case.syndrome)
      return "floating-point syndrome expected " + hex(test_case.syndrome) +
             " actual " + hex(outcome.syndrome);
    void *translated = nullptr;
    if (darwin_uml_static_vm_translate(generation, code_base,
                                       DARWIN_UML_STATIC_EXECUTE,
                                       &translated) !=
            DARWIN_UML_STATIC_TRANSLATED ||
        translated == nullptr)
      return std::string{"code mapping disappeared"};
    if (std::memcmp(translated, installed_.data(), installed_.size()) != 0)
      return std::string{"unexpected code-page write"};
    for (const auto &region : regions)
      if (auto failure = region->check())
        return failure;
    return std::nullopt;
  }

  /* Names the case by record index and by the producer check ordinal in
   * each page profile that exported it. */
  void report(const Record &record, std::size_t index, Lane lane,
              std::string_view message) const {
    const auto &test_case = record.cases[index];
    const auto &program = record.programs[test_case.program];
    std::string origin{record.producer};
    if (test_case.ordinal_16k != 0U)
      origin += " 16k check #" + std::to_string(test_case.ordinal_16k);
    if (test_case.ordinal_4k != 0U)
      origin += " 4k check #" + std::to_string(test_case.ordinal_4k);
    std::fprintf(stderr,
                 "FAIL %.*s case %zu \"%.*s\" (%s) entry %#010x %.*s: %.*s\n",
                 static_cast<int>(record.name.size()), record.name.data(),
                 index, static_cast<int>(test_case.name.size()),
                 test_case.name.data(), origin.c_str(), program.entry_word,
                 static_cast<int>(lane_name(lane).size()),
                 lane_name(lane).data(), static_cast<int>(message.size()),
                 message.data());
  }

  const Corpus &corpus_;
  const Options &options_;
  const ::uml::platform::HostCapabilities &host_;
  AddressSpace address_space_;
  SharedBacking code_backing_;
  GuestMapping code_mapping_;
  StaticGuestMemory memory_;
  std::array<std::byte, page_size> page_{};
  std::array<std::byte, page_size> installed_{};
  Counts counts_;
};

void usage() {
  std::fprintf(
      stderr,
      "usage: uml_arm64_instruction_corpus_tests PACK [options]\n"
      "  --record TEXT      run records whose name contains TEXT (repeatable)\n"
      "  --producer NAME    run records of one producer CTest (repeatable)\n"
      "  --case TEXT        run cases whose name contains TEXT (repeatable)\n"
      "  --ordinal N        run the case exported by producer check N of this\n"
      "                     page profile (repeatable, with --producer)\n"
      "  --engine NAME      run only the gadgets or jit lane\n"
      "  --max-failures N   stop after N failures (default 25, 0: never)\n"
      "  --fail-fast        stop at the first failure\n"
      "  --list             list the selected records without running them\n");
}

[[nodiscard]] std::optional<Options> parse_options(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    const auto value = [&]() -> std::optional<std::string_view> {
      if (index + 1 >= argc)
        return std::nullopt;
      return std::string_view{argv[++index]};
    };
    if (argument == "--record" || argument == "--producer" ||
        argument == "--case" || argument == "--ordinal" ||
        argument == "--engine" || argument == "--max-failures") {
      const auto text = value();
      if (!text.has_value())
        return std::nullopt;
      if (argument == "--record")
        options.records.push_back(*text);
      else if (argument == "--producer")
        options.producers.push_back(*text);
      else if (argument == "--case")
        options.cases.push_back(*text);
      else if (argument == "--engine") {
        if (*text != "gadgets" && *text != "jit")
          return std::nullopt;
        options.run_gadgets = *text == "gadgets";
        options.run_jit = *text == "jit";
      } else {
        char *end = nullptr;
        const auto number = std::strtoull(argv[index], &end, 10);
        if (end == argv[index] || *end != '\0')
          return std::nullopt;
        if (argument == "--ordinal") {
          if (number == 0U || number > 0xffff'ffffULL)
            return std::nullopt;
          options.ordinals.push_back(static_cast<std::uint32_t>(number));
        } else {
          options.max_failures = static_cast<std::size_t>(number);
        }
      }
    } else if (argument == "--fail-fast") {
      options.max_failures = 1;
    } else if (argument == "--list") {
      options.list = true;
    } else if (!argument.starts_with("--") && options.pack == nullptr) {
      options.pack = argv[index];
    } else {
      return std::nullopt;
    }
  }
  if (options.pack == nullptr)
    return std::nullopt;
  return options;
}

[[nodiscard]] bool record_selected(const Record &record,
                                   const Options &options) {
  if ((record.profiles & profile_bit) == 0U)
    return false;
  if (!options.producers.empty() &&
      std::ranges::find(options.producers, record.producer) ==
          options.producers.end())
    return false;
  if (!options.records.empty() &&
      std::ranges::none_of(options.records, [&](std::string_view text) {
        return record.name.find(text) != std::string_view::npos;
      }))
    return false;
  return true;
}

} // namespace

int main(int argc, char **argv) {
  const auto options = parse_options(argc, argv);
  if (!options.has_value()) {
    usage();
    return 2;
  }
  try {
    const auto started = std::chrono::steady_clock::now();
    const auto corpus = load_corpus(options->pack);
    std::vector<const Record *> records;
    for (const auto &record : corpus.records)
      if (record_selected(record, *options))
        records.push_back(&record);
    /* One engine set per option combination; pack order within each. */
    std::stable_sort(records.begin(), records.end(),
                     [](const Record *left, const Record *right) {
                       return left->options < right->options;
                     });
    if (options->list) {
      for (const auto *record : records)
        std::printf("%.*s\t%zu cases\n", static_cast<int>(record->name.size()),
                    record->name.data(), record->cases.size());
      return 0;
    }
    if (records.empty()) {
      std::fprintf(stderr, "no corpus record matches the selection\n");
      return 2;
    }
    /* The runner owns a full software TLB; keep it off the main stack. */
    const auto runner = std::make_unique<Runner>(corpus, *options);
    const bool completed = runner->run(records);
    const auto &counts = runner->counts();
    const auto elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    std::printf("arm64 instruction corpus (%zu KiB pages): records=%zu "
                "cases=%zu gadgets=%zu gadgets-skipped-by-host-feature=%zu "
                "jit=%zu jit-skipped-by-host-feature=%zu "
                "jit-unavailable=%zu failures=%zu%s %.1fs\n",
                page_size / 1024U, counts.records, counts.cases, counts.gadgets,
                counts.gadgets_skipped_host_feature, counts.jit,
                counts.jit_skipped_host_feature, counts.jit_unavailable,
                counts.failures,
                completed ? "" : " (stopped at the failure limit)", elapsed);
    if (!counts.missing_features.empty()) {
      std::printf("jit host-feature skips:");
      for (const auto &[feature, count] : counts.missing_features)
        std::printf(" %.*s=%zu", static_cast<int>(feature.size()),
                    feature.data(), count);
      std::printf("\n");
    }
    return counts.failures == 0U ? 0 : 1;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "arm64 instruction corpus: %s\n", error.what());
    return 2;
  }
}
