// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"
#include "arm64_instruction_evidence.hpp"
#include "arm64_instruction_materialization.hpp"
#include "instruction_evidence_digest.hpp"
#include "native_hardware_oracle.hpp"
#include "native_hardware_oracle_evidence.hpp"
#include "native_hardware_oracle_selection.hpp"

#include "cpu/engine.hpp"
#include "cpu/arm64/engine_execution.hpp"
#include "cpu/arm64/gadgets/executor.hpp"
#include "platform/apple/host_features.hpp"
#include "cpu/engine_factory.hpp"
#include "memory/static_guest_memory.hpp"

#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace uml::emulator::tests {
namespace {

using ::uml::memory::StaticGuestMemory;

constexpr std::uint64_t code_base = engine_test_code_base;
constexpr std::size_t execution_limit = 64;
constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

using Engine = ::uml::cpu::EngineKind;

struct Outcome final {
  ExpectedEvent event{ExpectedEvent::fatal};
  arm64::CpuState cpu;
  std::size_t instructions{};
  arm64::MemoryFault fault{};
  std::uint32_t syndrome{};
};

[[noreturn]] void
fail(std::string_view test_name, std::string_view engine,
     std::uint32_t instruction, std::string_view message,
     const std::source_location &location = std::source_location::current()) {
  std::fprintf(stderr,
               "%.*s: %.*s failed for instruction %#010x: %.*s at %s:%u\n",
               static_cast<int>(test_name.size()), test_name.data(),
               static_cast<int>(engine.size()), engine.data(), instruction,
               static_cast<int>(message.size()), message.data(),
               location.file_name(), location.line());
  std::abort();
}

[[nodiscard]] constexpr std::string_view engine_name(Engine engine) noexcept {
  switch (engine) {
  case Engine::interpreter:
    return "interpreter";
  case Engine::gadgets:
    return "gadgets";
  case Engine::jit:
    return "jit";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view
event_name(ExpectedEvent event) noexcept {
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

[[nodiscard]] constexpr std::size_t
default_instruction_count(const InstructionCase &test_case) noexcept {
  if (test_case.event == ExpectedEvent::system_call) {
    return (test_case.instruction & 0xffe0'001fU) == 0xd400'0001U ? 1U : 2U;
  }
  if (test_case.event == ExpectedEvent::breakpoint)
    return 1U;
  return 0U;
}

[[nodiscard]] constexpr std::optional<std::size_t>
instruction_count_expectation(const InstructionCase &test_case) noexcept {
  if (test_case.expected_instructions.has_value())
    return test_case.expected_instructions;
  if (test_case.event == ExpectedEvent::instruction_budget)
    return std::nullopt;
  return default_instruction_count(test_case);
}

using ::uml::emulator::tests::instruction_evidence::Digest;

[[nodiscard]] std::uint8_t expected_tag_at(
    const MemoryRegion &region, std::size_t offset) noexcept {
  std::uint8_t result{};
  for (const auto tag : region.initial_tags)
    if (tag.offset == offset)
      result = tag.value;
  for (const auto tag : region.expected_tags)
    if (tag.offset == offset)
      result = tag.value;
  return result;
}

[[nodiscard]] std::uint8_t initial_tag_at(
    const MemoryRegion &region, std::size_t offset) noexcept {
  std::uint8_t result{};
  for (const auto tag : region.initial_tags)
    if (tag.offset == offset)
      result = tag.value;
  return result;
}

[[nodiscard]] std::uint64_t
memory_initial_digest(std::span<const MemoryRegion> regions) noexcept {
  if (regions.empty())
    return instruction_evidence::empty_memory_digest();
  Digest digest;
  digest.scalar(regions.size());
  for (const auto &region : regions) {
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
memory_expectation_digest(std::span<const MemoryRegion> regions) noexcept {
  if (regions.empty())
    return instruction_evidence::empty_memory_digest();
  Digest digest;
  digest.scalar(regions.size());
  for (const auto &region : regions) {
    digest.scalar(region.address);
    digest.scalar(region.initial.size());
    digest.boolean(region.readable);
    digest.boolean(region.writable);
    digest.boolean(region.executable);
    digest.boolean(region.guest_code);
    digest.boolean(region.guest_writable);
    const auto expected =
        region.expected.empty() ? region.initial : region.expected;
    digest.bytes(expected.data(), expected.size());
    for (std::size_t offset = 0; offset < region.initial.size(); offset += 16U)
      digest.scalar(expected_tag_at(region, offset));
  }
  return digest.value();
}

[[nodiscard]] std::string retirement_expectation(
    const ProgramCase &test_case) {
  if (test_case.expected_instructions.has_value())
    return "exact:" + std::to_string(*test_case.expected_instructions);
  if (test_case.expected_instruction_pattern.has_value()) {
    const auto &pattern = *test_case.expected_instruction_pattern;
    return "pattern:" + std::to_string(pattern.minimum) + ":" +
           std::to_string(pattern.step) + ":" +
           (pattern.maximum.has_value() ? std::to_string(*pattern.maximum)
                                        : "unbounded");
  }
  return test_case.event == ExpectedEvent::instruction_budget
             ? "engine-dependent"
             : "interpreter-equal";
}

[[nodiscard]] std::string fault_expectation(
    const ProgramCase &test_case) {
  if (!test_case.compare_fault)
    return "none";
  const char *access{};
  switch (test_case.fault.access) {
  case arm64::MemoryAccess::read:
    access = "read";
    break;
  case arm64::MemoryAccess::write:
    access = "write";
    break;
  case arm64::MemoryAccess::execute:
    access = "execute";
    break;
  }
  char result[40]{};
  std::snprintf(result, sizeof(result), "%s:%016llx", access,
                static_cast<unsigned long long>(test_case.fault.address));
  return result;
}

[[nodiscard]] std::string state_oracle_identity(
    const ProgramCase &test_case,
    const std::source_location &location) {
  if (test_case.validator == nullptr)
    return "exact";
  const std::string_view path{location.file_name()};
  const auto marker = std::string_view{"tests/"};
  std::string source;
  if (path.starts_with(marker)) {
    source = path;
  } else if (const auto found = path.rfind(std::string{"/"} +
                                           std::string{marker});
             found != std::string_view::npos) {
    source = path.substr(found + 1U);
  } else {
    source = std::filesystem::path{path}.filename().string();
  }
  return "custom-validator@" +
         source + ":" + std::to_string(location.line());
}

[[nodiscard]] constexpr ExpectedEvent
event_for(::uml::cpu::ExitReason reason) noexcept {
	switch (reason) {
	case ::uml::cpu::ExitReason::system_call:
		return ExpectedEvent::system_call;
	case ::uml::cpu::ExitReason::memory_fault:
		return ExpectedEvent::memory_fault;
	case ::uml::cpu::ExitReason::gcs_fault:
		return ExpectedEvent::gcs_fault;
	case ::uml::cpu::ExitReason::illegal_instruction:
		return ExpectedEvent::illegal_instruction;
	case ::uml::cpu::ExitReason::breakpoint:
		return ExpectedEvent::breakpoint;
	case ::uml::cpu::ExitReason::floating_point_trap:
		return ExpectedEvent::floating_point_trap;
	case ::uml::cpu::ExitReason::pc_alignment:
		return ExpectedEvent::pc_alignment;
	case ::uml::cpu::ExitReason::instruction_budget:
	case ::uml::cpu::ExitReason::interrupt:
		return ExpectedEvent::instruction_budget;
	case ::uml::cpu::ExitReason::fatal:
		return ExpectedEvent::fatal;
	}
	return ExpectedEvent::fatal;
}

class Backing final {
public:
  explicit Backing(std::size_t size) {
    char path[] = "/private/tmp/uml-engine-test.XXXXXX";
    descriptor_ = mkstemp(path);
    if (descriptor_ < 0)
      fail("fixture", "setup", 0, std::strerror(errno));
    if (unlink(path) != 0 ||
        ftruncate(descriptor_, static_cast<off_t>(size)) != 0)
      fail("fixture", "setup", 0, std::strerror(errno));
  }

  ~Backing() {
    if (descriptor_ >= 0)
      (void)close(descriptor_);
  }

  Backing(const Backing &) = delete;
  Backing &operator=(const Backing &) = delete;

  [[nodiscard]] int descriptor() const noexcept { return descriptor_; }

  void read(std::span<std::byte> bytes, std::size_t offset = 0) const {
    std::size_t completed = 0;
    while (completed != bytes.size()) {
      const auto result =
          pread(descriptor_, bytes.data() + completed, bytes.size() - completed,
                static_cast<off_t>(offset + completed));
      if (result < 0 && errno == EINTR)
        continue;
      if (result <= 0)
        fail("fixture", "snapshot", 0,
             result < 0 ? std::strerror(errno) : "short backing read");
      completed += static_cast<std::size_t>(result);
    }
  }

  void write(std::span<const std::byte> bytes, std::size_t offset = 0) const {
    std::size_t completed = 0;
    while (completed != bytes.size()) {
      const auto result = pwrite(descriptor_, bytes.data() + completed,
                                 bytes.size() - completed,
                                 static_cast<off_t>(offset + completed));
      if (result < 0 && errno == EINTR)
        continue;
      if (result <= 0)
        fail("fixture", "restore", 0,
             result < 0 ? std::strerror(errno) : "short backing write");
      completed += static_cast<std::size_t>(result);
    }
  }

private:
  int descriptor_{-1};
};

class AddressSpace final {
public:
  explicit AddressSpace(unsigned long generation) : generation_{generation} {
    if (darwin_uml_static_vm_activate(generation_) != 0)
      fail("fixture", "mapping", 0, "cannot activate address space");
  }

  ~AddressSpace() {
    if (darwin_uml_static_vm_destroy(generation_) != 0)
      fail("fixture", "mapping", 0, "cannot destroy address space");
  }

  AddressSpace(const AddressSpace &) = delete;
  AddressSpace &operator=(const AddressSpace &) = delete;

private:
  unsigned long generation_;
};

class Mapping final {
public:
  Mapping(unsigned long generation, std::uint64_t address, Backing &backing,
          std::size_t size, int protection, bool guest_code,
          bool guest_writable)
      : generation_{generation}, address_{address}, size_{size} {
    if (darwin_uml_static_vm_map_generation(
            generation_, reinterpret_cast<void *>(address_),
            backing.descriptor(), 0, size_, protection, guest_code ? 1 : 0,
            guest_writable ? 1 : 0) != 0)
      fail("fixture", "mapping", 0, "cannot map test region");
  }

  ~Mapping() {
    if (darwin_uml_static_vm_unmap_generation(
            generation_, reinterpret_cast<void *>(address_), size_) != 0)
      fail("fixture", "mapping", 0, "cannot unmap test region");
  }

  Mapping(const Mapping &) = delete;
  Mapping &operator=(const Mapping &) = delete;

private:
  unsigned long generation_;
  std::uint64_t address_;
  std::size_t size_;
};

[[nodiscard]] int region_protection(const MemoryRegion &region) noexcept {
  return (region.readable ? PROT_READ : 0) |
         (region.writable ? PROT_WRITE : 0) |
         (region.executable ? PROT_EXEC : 0);
}

class CaseRegion final {
public:
  CaseRegion(unsigned long generation, const MemoryRegion &region)
      : generation_{generation}, region_{region},
        backing_{region.initial.size()}, mapping_{generation,
                                                  region.address,
                                                  backing_,
                                                  region.initial.size(),
                                                  region_protection(region),
                                                  region.guest_code,
                                                  region.guest_writable},
        actual_(region.initial.size()),
        initial_tags_(region.initial.size() / 16),
        expected_tags_(region.initial.size() / 16) {
    for (const auto tag : region_.initial_tags)
      initial_tags_[tag.offset / 16] = tag.value;
    expected_tags_ = initial_tags_;
    for (const auto tag : region_.expected_tags)
      expected_tags_[tag.offset / 16] = tag.value;
    restore();
  }

  CaseRegion(const CaseRegion &) = delete;
  CaseRegion &operator=(const CaseRegion &) = delete;

  void restore() const {
    backing_.write(region_.initial);
    if (region_.initial_tags.empty() && region_.expected_tags.empty())
      return;
    for (std::size_t index = 0; index < initial_tags_.size(); ++index)
      if (darwin_uml_static_vm_store_allocation_tag(
              generation_, region_.address + index * 16,
              initial_tags_[index]) !=
          DARWIN_UML_STATIC_TRANSLATED)
        fail("fixture", "tag-restore", 0, "cannot restore allocation tag");
  }

  void require_expected(std::string_view test_name, std::string_view engine,
                        std::uint32_t instruction) {
    backing_.read(actual_);
    const auto expected =
        region_.expected.empty() ? region_.initial : region_.expected;
    for (std::size_t offset = 0; offset < actual_.size(); ++offset) {
      if (actual_[offset] == expected[offset])
        continue;
      std::fprintf(stderr,
                   "memory mismatch at %#llx: expected=%02x actual=%02x\n",
                   static_cast<unsigned long long>(region_.address + offset),
                   std::to_integer<unsigned>(expected[offset]),
                   std::to_integer<unsigned>(actual_[offset]));
      fail(test_name, engine, instruction, "unexpected data-region contents");
    }
    if (region_.initial_tags.empty() && region_.expected_tags.empty())
      return;
    for (std::size_t index = 0; index < expected_tags_.size(); ++index) {
      std::uint8_t actual_tag{};
      if (darwin_uml_static_vm_load_allocation_tag(
              generation_, region_.address + index * 16, &actual_tag) !=
              DARWIN_UML_STATIC_TRANSLATED ||
          actual_tag != expected_tags_[index]) {
        std::fprintf(
            stderr, "allocation-tag mismatch at %#llx: expected=%x actual=%x\n",
            static_cast<unsigned long long>(region_.address + index * 16),
            static_cast<unsigned>(expected_tags_[index]),
            static_cast<unsigned>(actual_tag));
        fail(test_name, engine, instruction,
             "unexpected allocation-tag contents");
      }
    }
  }

private:
  unsigned long generation_;
  MemoryRegion region_;
  Backing backing_;
  Mapping mapping_;
  std::vector<std::byte> actual_;
  std::vector<std::uint8_t> initial_tags_;
  std::vector<std::uint8_t> expected_tags_;
};

[[nodiscard]] bool ranges_overlap(std::uint64_t first_address,
                                  std::size_t first_size,
                                  std::uint64_t second_address,
                                  std::size_t second_size) noexcept {
  const auto first_end = first_address + first_size;
  const auto second_end = second_address + second_size;
  return first_address < second_end && second_address < first_end;
}

void validate_allocation_tags(std::string_view name, std::uint32_t instruction,
                              std::size_t size,
                              std::span<const AllocationTag> initial,
                              std::span<const AllocationTag> expected) {
  const auto validate = [&](std::span<const AllocationTag> tags) {
    for (std::size_t index = 0; index < tags.size(); ++index) {
      if (tags[index].offset >= size || tags[index].offset % 16 != 0 ||
          tags[index].value > 15)
        fail(name, "setup", instruction, "invalid allocation tag");
      for (std::size_t prior = 0; prior < index; ++prior)
        if (tags[prior].offset == tags[index].offset)
          fail(name, "setup", instruction, "duplicate allocation tag");
    }
  };
  validate(initial);
  validate(expected);
}

void validate_memory_regions(std::string_view name, std::uint32_t instruction,
                             std::span<const MemoryRegion> memory) {
  for (std::size_t index = 0; index < memory.size(); ++index) {
    const auto &region = memory[index];
    const auto size = region.initial.size();
    if (size == 0 || size % DARWIN_UML_GUEST_PAGE_SIZE != 0 ||
        region.address % DARWIN_UML_GUEST_PAGE_SIZE != 0 ||
        region.address > std::numeric_limits<std::uint64_t>::max() - size ||
        (!region.expected.empty() &&
         region.expected.size() != region.initial.size()))
      fail(name, "setup", instruction, "invalid memory-region shape");
    validate_allocation_tags(name, instruction, size, region.initial_tags,
                             region.expected_tags);
    if (ranges_overlap(region.address, size, code_base,
                       DARWIN_UML_GUEST_PAGE_SIZE))
      fail(name, "setup", instruction, "memory region overlaps code");
    for (std::size_t prior = 0; prior < index; ++prior)
      if (ranges_overlap(region.address, size, memory[prior].address,
                         memory[prior].initial.size()))
        fail(name, "setup", instruction, "memory regions overlap");
  }
}

[[nodiscard]] unsigned long next_generation() noexcept {
  static std::atomic<unsigned long> generation{0x10000};
  return generation.fetch_add(1, std::memory_order_relaxed);
}

class ExecutionScope final {
public:
  ExecutionScope(unsigned long generation, std::uint64_t pc)
      : generation_{generation},
        entered_{darwin_uml_static_vm_execution_begin(generation, pc) == 0} {}

  ~ExecutionScope() {
    if (entered_)
      (void)darwin_uml_static_vm_execution_end(generation_);
  }

  [[nodiscard]] bool entered() const noexcept { return entered_; }

private:
  unsigned long generation_;
  bool entered_;
};

void install_program(
    std::string_view name, std::uint32_t diagnostic_instruction,
    std::span<const ProgramWord> code,
    std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> &page,
    std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> &installed_page) {
  page.fill(std::byte{});
  std::array<bool, DARWIN_UML_GUEST_PAGE_SIZE / sizeof(std::uint32_t)> used{};
  if (code.empty())
    fail(name, "setup", diagnostic_instruction, "empty test program");
  for (const auto word : code) {
    if (word.offset % sizeof(word.instruction) != 0 ||
        word.offset > page.size() - sizeof(word.instruction) ||
        used[word.offset / sizeof(word.instruction)])
      fail(name, "setup", diagnostic_instruction,
           "invalid test-program layout");
    used[word.offset / sizeof(word.instruction)] = true;
    std::memcpy(page.data() + word.offset, &word.instruction,
                sizeof(word.instruction));
  }
  std::size_t first = 0;
  while (first != page.size() && page[first] == installed_page[first])
    ++first;
  if (first == page.size())
    return;
  std::size_t end = page.size();
  while (end != first && page[end - 1] == installed_page[end - 1])
    --end;
  if (darwin_uml_static_vm_copy_from_buffer(
          code_base + first, page.data() + first, end - first) != 0)
    fail(name, "restore", diagnostic_instruction, "cannot restore code page");
  installed_page = page;
}

[[nodiscard]] Outcome run_engine(
    Engine selected, const arm64::CpuState &initial,
    StaticGuestMemory &memory, unsigned long generation,
    ::uml::cpu::CpuEngine &engine) {
  Outcome outcome{};
  outcome.cpu = initial;
  const auto sequences = darwin_uml_static_vm_sequences(generation);
  memory.prepare(generation, sequences.mapping_changes);

  const auto run = [&] {
    ::uml::cpu::ExecutionRequest request{
        .cpu = outcome.cpu,
        .memory = memory,
        .translated_memory = &memory,
        .generation = generation,
        .instruction_budget = execution_limit,
    };
    const auto result = engine.run(request);
    outcome.event = event_for(result.reason);
    outcome.instructions = result.retired_instructions;
    outcome.fault = {result.fault.address, result.fault.access};
    outcome.syndrome = result.syndrome;
  };

  if (selected == Engine::interpreter) {
    ExecutionScope scope{generation, outcome.cpu.pc};
    if (!scope.entered())
      return outcome;
    run();
  } else {
    run();
  }
  return outcome;
}

[[nodiscard]] Outcome run_forced_gadget_executor(
    const arm64::CpuState &initial, StaticGuestMemory &memory,
    unsigned long generation,
    ::uml::cpu::arm64::gadgets::GadgetExecutor &executor) {
  Outcome outcome{};
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
  const auto result =
      ::uml::cpu::arm64::detail::run_translated(executor, request);
  outcome.event = event_for(result.reason);
  outcome.instructions = result.retired_instructions;
  outcome.fault = {result.fault.address, result.fault.access};
  outcome.syndrome = result.syndrome;
  return outcome;
}

void require_equal(bool equal, const arm64::CpuState &expected,
                   const arm64::CpuState &actual, std::string_view field,
                   std::string_view test_name, std::string_view engine,
                   std::uint32_t instruction) {
  if (equal)
    return;
  if (field == "general registers") {
    for (std::size_t index = 0; index < expected.registers.size(); ++index)
      if (expected.registers[index] != actual.registers[index]) {
        std::fprintf(
            stderr, "expected x%zu=%#llx actual=%#llx\n", index,
            static_cast<unsigned long long>(expected.registers[index]),
            static_cast<unsigned long long>(actual.registers[index]));
        break;
      }
  } else if (field == "vector registers") {
    bool reported = false;
    for (std::size_t index = 0;
         index < expected.vectors.size() && !reported; ++index)
      for (std::size_t byte = 0;
           byte < expected.vectors[index].bytes.size(); ++byte)
        if (expected.vectors[index].bytes[byte] !=
            actual.vectors[index].bytes[byte]) {
          std::fprintf(stderr,
                       "expected v%zu[%zu]=%02x actual=%02x\n", index, byte,
                       std::to_integer<unsigned>(
                           expected.vectors[index].bytes[byte]),
                       std::to_integer<unsigned>(
                           actual.vectors[index].bytes[byte]));
          reported = true;
          break;
        }
  } else if (field == "pc")
    std::fprintf(stderr, "expected pc=%#llx actual=%#llx\n",
                 static_cast<unsigned long long>(expected.pc),
                 static_cast<unsigned long long>(actual.pc));
  else if (field == "sp")
    std::fprintf(stderr, "expected sp=%#llx actual=%#llx\n",
                 static_cast<unsigned long long>(expected.sp),
                 static_cast<unsigned long long>(actual.sp));
  else if (field == "nzcv")
    std::fprintf(stderr, "expected nzcv=%#x actual=%#x\n", expected.nzcv,
                 actual.nzcv);
  else if (field == "fpcr")
    std::fprintf(stderr, "expected fpcr=%#x actual=%#x\n", expected.fpcr,
                 actual.fpcr);
  else if (field == "fpsr")
    std::fprintf(stderr, "expected fpsr=%#x actual=%#x\n", expected.fpsr,
                 actual.fpsr);
  else if (field == "fpmr")
    std::fprintf(stderr, "expected fpmr=%#llx actual=%#llx\n",
                 static_cast<unsigned long long>(expected.fpmr),
                 static_cast<unsigned long long>(actual.fpmr));
  else if (field == "tls")
    std::fprintf(stderr, "expected tls=%#llx actual=%#llx\n",
                 static_cast<unsigned long long>(expected.tls),
                 static_cast<unsigned long long>(actual.tls));
  fail(test_name, engine, instruction, field);
}

[[nodiscard]] bool vectors_equal(const arm64::CpuState &expected,
                                 const arm64::CpuState &actual) noexcept {
  for (std::size_t index = 0; index < expected.vectors.size(); ++index)
    if (expected.vectors[index].bytes != actual.vectors[index].bytes)
      return false;
  return true;
}

} // namespace

bool host_supports(NativeRequirements requirements) noexcept {
  return ::uml::platform::apple::probed_host_capabilities().supports(requirements.features());
}

void require_architecture_equal(const arm64::CpuState &expected,
                                const arm64::CpuState &actual,
                                std::string_view case_name,
                                std::string_view engine,
                                std::uint32_t instruction,
                                const std::source_location location) {
  (void)location;
  require_equal(expected.registers == actual.registers, expected, actual,
                "general registers", case_name, engine, instruction);
  require_equal(expected.sp == actual.sp, expected, actual, "sp", case_name,
                engine, instruction);
  require_equal(expected.pc == actual.pc, expected, actual, "pc", case_name,
                engine, instruction);
  require_equal(expected.nzcv == actual.nzcv, expected, actual, "nzcv",
                case_name, engine, instruction);
  require_equal(vectors_equal(expected, actual), expected, actual,
                "vector registers", case_name, engine, instruction);
  require_equal(expected.fpcr == actual.fpcr, expected, actual, "fpcr",
                case_name, engine, instruction);
  require_equal(expected.fpsr == actual.fpsr, expected, actual, "fpsr",
                case_name, engine, instruction);
  require_equal(expected.fpmr == actual.fpmr, expected, actual, "fpmr",
                case_name, engine, instruction);
  require_equal(expected.tls == actual.tls, expected, actual, "tls", case_name,
                engine, instruction);
  require_equal(expected.mte_enabled == actual.mte_enabled &&
                    expected.mte_exclude == actual.mte_exclude &&
                    expected.mte_tag_choices == actual.mte_tag_choices &&
                    expected.mte_random_tags == actual.mte_random_tags &&
                    expected.mte_seed == actual.mte_seed &&
                    expected.mte_tag == actual.mte_tag &&
                    expected.mte_random_state == actual.mte_random_state,
                expected, actual, "tag state", case_name, engine, instruction);
  require_equal(expected.cpa_enabled == actual.cpa_enabled &&
                    expected.cpa_multiply_enabled ==
                        actual.cpa_multiply_enabled &&
                    expected.accdata == actual.accdata,
                expected, actual, "checked arithmetic state", case_name, engine,
                instruction);
  require_equal(expected.pstate_ssbs == actual.pstate_ssbs &&
                    expected.pstate_dit == actual.pstate_dit &&
                    expected.pstate_tco == actual.pstate_tco &&
                    expected.pstate_sm == actual.pstate_sm &&
                    expected.pstate_za == actual.pstate_za &&
                    expected.pstate_daif_access == actual.pstate_daif_access &&
                    expected.pstate_daif == actual.pstate_daif,
                expected, actual, "pstate", case_name, engine, instruction);
  require_equal(expected.exclusive_address == actual.exclusive_address &&
                    expected.exclusive_key == actual.exclusive_key &&
                    expected.exclusive_value == actual.exclusive_value &&
                    expected.exclusive_value2 == actual.exclusive_value2 &&
                    expected.exclusive_version == actual.exclusive_version &&
                    expected.exclusive_size == actual.exclusive_size,
                expected, actual, "exclusive monitor", case_name, engine,
                instruction);
  require_equal(expected.gcs_pointer == actual.gcs_pointer &&
                    expected.gcs_implemented == actual.gcs_implemented &&
                    expected.gcs_push_enabled == actual.gcs_push_enabled &&
                    expected.gcs_store_enabled == actual.gcs_store_enabled,
                expected, actual, "guarded control stack", case_name, engine,
                instruction);
  require_equal(
      expected.pointer_auth_keys == actual.pointer_auth_keys &&
          expected.pointer_authentication_enabled ==
              actual.pointer_authentication_enabled,
      expected, actual, "pointer-authentication state", case_name, engine,
      instruction);
}

namespace {

[[nodiscard]] bool has_feature(const ArmFeatureSet &features,
                               const ArmFeature feature) noexcept {
  return features.test(static_cast<std::size_t>(feature));
}

[[nodiscard]] bool native_requires_unavailable_feature_state(
    const ArmFeatureSet &features) noexcept {
  for (const auto feature : {
           ArmFeature::cpa,
           ArmFeature::d128,
           ArmFeature::dit,
           ArmFeature::f8f16mm,
           ArmFeature::f8f32mm,
           ArmFeature::fp8,
           ArmFeature::fp8dot2,
           ArmFeature::fp8dot4,
           ArmFeature::fp8fma,
           ArmFeature::gcie,
           ArmFeature::gcs,
           ArmFeature::ls64_accdata,
           ArmFeature::mte,
           ArmFeature::mte2,
           ArmFeature::mtetc,
           ArmFeature::nmi,
           ArmFeature::pauth,
           ArmFeature::pauth_lr,
           ArmFeature::sme,
           ArmFeature::ssbs,
           ArmFeature::sysinstr128,
           ArmFeature::sysreg128,
           ArmFeature::uao,
       })
    if (has_feature(features, feature))
      return true;
  return false;
}

[[nodiscard]] bool native_unrepresented_state_changes(
    const arm64::CpuState &initial,
    const arm64::CpuState &expected) noexcept {
  return initial.fpmr != expected.fpmr || initial.tls != expected.tls ||
      initial.mte_enabled != expected.mte_enabled ||
      initial.mte_exclude != expected.mte_exclude ||
      initial.mte_tag_choices != expected.mte_tag_choices ||
      initial.mte_random_tags != expected.mte_random_tags ||
      initial.mte_seed != expected.mte_seed ||
      initial.mte_tag != expected.mte_tag ||
      initial.mte_random_state != expected.mte_random_state ||
      initial.cpa_enabled != expected.cpa_enabled ||
      initial.cpa_multiply_enabled != expected.cpa_multiply_enabled ||
      initial.accdata != expected.accdata ||
      initial.pstate_ssbs != expected.pstate_ssbs ||
      initial.pstate_dit != expected.pstate_dit ||
      initial.pstate_tco != expected.pstate_tco ||
      initial.pstate_sm != expected.pstate_sm ||
      initial.pstate_za != expected.pstate_za ||
      initial.pstate_daif_access != expected.pstate_daif_access ||
      initial.pstate_daif != expected.pstate_daif ||
      initial.exclusive_address != expected.exclusive_address ||
      initial.exclusive_key != expected.exclusive_key ||
      initial.exclusive_value != expected.exclusive_value ||
      initial.exclusive_value2 != expected.exclusive_value2 ||
      initial.exclusive_version != expected.exclusive_version ||
      initial.exclusive_size != expected.exclusive_size ||
      initial.gcs_pointer != expected.gcs_pointer ||
      initial.gcs_implemented != expected.gcs_implemented ||
      initial.gcs_push_enabled != expected.gcs_push_enabled ||
      initial.gcs_store_enabled != expected.gcs_store_enabled ||
      initial.pointer_auth_keys != expected.pointer_auth_keys ||
      initial.pointer_authentication_enabled !=
          expected.pointer_authentication_enabled;
}

[[nodiscard]] bool native_fp_state_is_round_trippable(
    const arm64::CpuState &initial, const arm64::CpuState &expected,
    const ::uml::cpu::HostCapabilities &host) noexcept {
  constexpr std::uint32_t nzcv_mask = 0xf000'0000U;
  constexpr std::uint32_t fpcr_mask = 0x07f7'9f07U;
  constexpr std::uint32_t fpsr_mask = 0xf800'009fU;
  constexpr std::uint32_t afp_control_mask = 0x0000'0007U;
  if (((initial.nzcv | expected.nzcv) & ~nzcv_mask) != 0U ||
      ((initial.fpcr | expected.fpcr) & ~fpcr_mask) != 0U ||
      ((initial.fpsr | expected.fpsr) & ~fpsr_mask) != 0U)
    return false;
  return host.has(ArmFeature::afp) ||
      ((initial.fpcr | expected.fpcr) & afp_control_mask) == 0U;
}

[[nodiscard]] constexpr bool native_literal_access(
    const std::uint32_t instruction) noexcept {
  return (instruction & 0x1f00'0000U) == 0x1800'0000U;
}

[[nodiscard]] constexpr bool native_pc_relative_address(
    const std::uint32_t instruction) noexcept {
  return (instruction & 0x1f00'0000U) == 0x1000'0000U;
}

[[nodiscard]] constexpr bool native_may_reference_platform_register(
    const std::uint32_t instruction) noexcept {
  return (instruction & 31U) == 18U ||
      ((instruction >> 5U) & 31U) == 18U ||
      ((instruction >> 10U) & 31U) == 18U ||
      ((instruction >> 16U) & 31U) == 18U;
}

[[nodiscard]] constexpr bool native_event_is_representable(
    const ExpectedEvent event) noexcept {
  switch (event) {
  case ExpectedEvent::system_call:
  case ExpectedEvent::memory_fault:
  case ExpectedEvent::illegal_instruction:
  case ExpectedEvent::breakpoint:
    return true;
  case ExpectedEvent::gcs_fault:
  case ExpectedEvent::floating_point_trap:
  case ExpectedEvent::pc_alignment:
  case ExpectedEvent::instruction_budget:
  case ExpectedEvent::fatal:
    return false;
  }
  return false;
}

[[nodiscard]] constexpr native_oracle::Event native_expected_event(
    const ExpectedEvent event) noexcept {
  switch (event) {
  case ExpectedEvent::system_call:
    return native_oracle::Event::completed;
  case ExpectedEvent::memory_fault:
    return native_oracle::Event::memory_fault;
  case ExpectedEvent::illegal_instruction:
    return native_oracle::Event::illegal_instruction;
  case ExpectedEvent::breakpoint:
    return native_oracle::Event::breakpoint;
  case ExpectedEvent::gcs_fault:
  case ExpectedEvent::floating_point_trap:
  case ExpectedEvent::pc_alignment:
  case ExpectedEvent::instruction_budget:
  case ExpectedEvent::fatal:
    return native_oracle::Event::none;
  }
  return native_oracle::Event::none;
}

[[nodiscard]] constexpr std::size_t native_expected_retirement(
    const ExpectedEvent event) noexcept {
  return event == ExpectedEvent::system_call ||
                 event == ExpectedEvent::breakpoint
             ? 1U
             : 0U;
}

struct NativeAddressOperands final {
  std::uint32_t input_register_mask{};
  std::uint32_t output_register_mask{};
  std::uint32_t output_pc_relative_register_mask{};
  bool input_stack_pointer{};
  bool output_stack_pointer{};
  bool unsupported{};
  bool unavailable_geometry{};
};

[[nodiscard]] bool native_declared_pointer(
    const std::uint64_t value,
    const std::span<const MemoryRegion> memory) noexcept {
  constexpr std::uint64_t low_address_mask = 0x00ff'ffff'ffff'ffffULL;
  const auto low = value & low_address_mask;
  if (low >= code_base &&
      low <= code_base + DARWIN_UML_GUEST_PAGE_SIZE)
    return true;
  for (const auto &region : memory) {
    if (region.initial.size() >
        std::numeric_limits<std::uint64_t>::max() - region.address)
      continue;
    const auto end = region.address + region.initial.size();
    if (low >= region.address && low <= end)
      return true;
  }
  return false;
}

[[nodiscard]] NativeAddressOperands native_address_operands(
    const InstructionCase &test_case) noexcept {
  NativeAddressOperands result;
  if (native_pc_relative_address(test_case.instruction)) {
    const auto destination = test_case.instruction & 31U;
    if (destination < 31U)
      result.output_pc_relative_register_mask |=
          std::uint32_t{1} << destination;
  }
  if ((test_case.instruction & 0xfc00'0000U) == 0x9400'0000U)
    result.output_pc_relative_register_mask |= std::uint32_t{1} << 30U;

  const bool accesses_memory = !test_case.memory.empty() ||
      test_case.event == ExpectedEvent::memory_fault;
  if (!accesses_memory)
    return result;
  if (native_literal_access(test_case.instruction) ||
      has_feature(test_case.native.features(), ArmFeature::mops)) {
    result.unsupported = true;
    return result;
  }

  /* Every non-literal A64 load/store allocation uses Rn at bits [9:5].
   * Register-offset operands and transfer data remain guest scalar values;
   * relocating those address-looking values would corrupt valid data. */
  const auto base = (test_case.instruction >> 5U) & 31U;
  if (base == 31U) {
    result.input_stack_pointer = true;
    result.output_stack_pointer = true;
    result.unavailable_geometry =
        !native_declared_pointer(test_case.initial.sp, test_case.memory);
  } else {
    result.input_register_mask |= std::uint32_t{1} << base;
    result.output_register_mask |= std::uint32_t{1} << base;
    result.unavailable_geometry = !native_declared_pointer(
        test_case.initial.registers[base], test_case.memory);
  }
  if (test_case.compare_fault)
    result.unavailable_geometry = result.unavailable_geometry ||
        !native_declared_pointer(test_case.fault.address, test_case.memory);
  return result;
}

[[nodiscard]] constexpr bool native_exclusive_access(
    const std::uint32_t instruction) noexcept {
  return (instruction & 0x3f00'0000U) == 0x0800'0000U;
}

[[nodiscard]] bool native_exact_fault_event_is_unrepresentable(
    const InstructionCase &test_case) noexcept {
  /* Darwin does not provide the accessed address in siginfo for every
   * alignment fault raised by an exclusive access. An in-range expected fault
   * is therefore not an exact native fault observation even though the signal
   * itself is safely contained in the child. */
  return test_case.event == ExpectedEvent::memory_fault &&
      test_case.compare_fault &&
      native_exclusive_access(test_case.instruction) &&
      native_declared_pointer(test_case.fault.address, test_case.memory);
}

class NativeMemoryImage final {
public:
  explicit NativeMemoryImage(const std::span<const MemoryRegion> source) {
    tags_.reserve(source.size());
    regions_.reserve(source.size());
    for (const auto &region : source) {
      auto &tags = tags_.emplace_back();
      tags.reserve(region.initial_tags.size());
      for (const auto tag : region.initial_tags)
        tags.push_back({.offset = tag.offset, .value = tag.value});
      regions_.push_back({
          .address = region.address,
          .initial = region.initial,
          .initial_tags = tags,
          .readable = region.readable,
          .writable = region.writable,
          .executable = region.executable,
          .capture_after_execution = true,
          .guest_code = region.guest_code,
          .guest_writable = region.guest_writable,
      });
    }
  }

  [[nodiscard]] std::span<const native_oracle::MemoryRegion>
  regions() const noexcept {
    return regions_;
  }

private:
  std::vector<std::vector<native_oracle::AllocationTag>> tags_;
  std::vector<native_oracle::MemoryRegion> regions_;
};

[[nodiscard]] bool native_memory_has_tags(
    const std::span<const MemoryRegion> memory) noexcept {
  return std::ranges::any_of(memory, [](const MemoryRegion &region) {
    return !region.initial_tags.empty() || !region.expected_tags.empty();
  });
}

void require_native_memory(
    const InstructionCase &test_case,
    const native_oracle::Observation &observation) {
  if (observation.memory.size() != test_case.memory.size())
    fail(test_case.name, "native-hardware", test_case.instruction,
         "memory observation count");
  for (std::size_t index = 0; index < test_case.memory.size(); ++index) {
    const auto &expected_region = test_case.memory[index];
    const auto &actual_region = observation.memory[index];
    const auto expected = expected_region.expected.empty()
        ? expected_region.initial
        : expected_region.expected;
    if (!actual_region.captured ||
        actual_region.address != expected_region.address ||
        !std::ranges::equal(actual_region.bytes, expected))
      fail(test_case.name, "native-hardware", test_case.instruction,
           "memory result");
  }
}

} // namespace

class EngineHarness::Implementation final {
public:
  explicit Implementation(EngineHarnessOptions options)
      : generation_{next_generation()}, address_space_{generation_},
        backing_{DARWIN_UML_GUEST_PAGE_SIZE},
        mapping_{generation_,
                 code_base,
                 backing_,
                 DARWIN_UML_GUEST_PAGE_SIZE,
                 PROT_READ | PROT_EXEC,
                 true,
                 false} {
    const ::uml::cpu::EngineConfiguration configuration{
        .enable_guarded_control_flow = options.enable_guarded_control_flow,
        .enable_pointer_authentication =
            options.enable_pointer_authentication,
        .host_capabilities = ::uml::platform::apple::probed_host_capabilities(),
    };
    interpreter_engine_ = ::uml::cpu::make_cpu_engine(
        ::uml::cpu::EngineKind::interpreter, configuration);
    const bool forced_gadget_path = options.force_software_checksum ||
        options.force_software_flags || options.force_software_sha3 ||
        options.force_software_fixed_transforms ||
        options.force_software_aes || options.force_software_pmull ||
        options.force_software_dotprod || options.force_software_i8mm;
    if (!forced_gadget_path) {
      gadget_engine_ = ::uml::cpu::make_cpu_engine(
          ::uml::cpu::EngineKind::gadgets, configuration);
    } else {
      forced_gadget_executor_ =
          std::make_unique<::uml::cpu::arm64::gadgets::GadgetExecutor>(
          ::uml::cpu::arm64::gadgets::BuildOptions{
            .enable_pointer_authentication =
                options.enable_pointer_authentication,
            .enable_guarded_control_flow = options.enable_guarded_control_flow,
            .force_software_checksum = options.force_software_checksum,
            .force_software_flags = options.force_software_flags,
            .force_software_sha3 = options.force_software_sha3,
            .force_software_fixed_transforms =
                options.force_software_fixed_transforms,
            .force_software_aes = options.force_software_aes,
            .force_software_pmull = options.force_software_pmull,
            .force_software_dotprod = options.force_software_dotprod,
            .force_software_i8mm = options.force_software_i8mm,
            .host_capabilities =
                ::uml::platform::apple::probed_host_capabilities(),
          });
    }
    jit_engine_ = ::uml::cpu::make_cpu_engine(
        ::uml::cpu::EngineKind::jit, configuration);
  }

  void check(const InstructionCase &test_case,
             const std::source_location &location) {
    const std::array code{
        ProgramWord{.offset = 0, .instruction = test_case.instruction},
        ProgramWord{.offset = sizeof(std::uint32_t),
                    .instruction = terminal_system_call},
    };
    auto expected = test_case.expected;
    const bool direct_system_call =
        test_case.event == ExpectedEvent::system_call &&
        (test_case.instruction & 0xffe0'001fU) == 0xd400'0001U;
    expected.pc =
        test_case.event == ExpectedEvent::system_call
            ? code_base + (direct_system_call ? 1U : 2U) *
                              sizeof(std::uint32_t)
            : code_base;
    const std::array evidence_words{test_case.instruction};
    check_program({
        .name = test_case.name,
        .code = std::span<const ProgramWord>{code},
        .initial = test_case.initial,
        .expected = expected,
        .event = test_case.event,
        .native = test_case.native,
        .fault = test_case.fault,
        .compare_fault = test_case.compare_fault,
        .syndrome = test_case.syndrome,
        .invalid_encoding = test_case.invalid_encoding,
        .memory = test_case.memory,
        .validator = test_case.validator,
        .validator_relation_id = test_case.validator_relation_id,
        .execution_context = test_case.execution_context,
        .before_engine = test_case.before_engine,
        .before_engine_effect_id = test_case.before_engine_effect_id,
        .after_engine = test_case.after_engine,
        .after_engine_effect_id = test_case.after_engine_effect_id,
        .expected_instructions = instruction_count_expectation(test_case),
    }, evidence_words, InstructionNativeScope::instruction, location);

    auto selected = native_oracle::select_native_allocations(
        test_case.instruction, native_oracle::SelectionScope::instruction,
        location);
    std::vector<std::string_view> oracle_case_ids{
        selected.allocation_status_ids.begin(),
        selected.allocation_status_ids.end()};
    if (!test_case.native_oracle_case_id.empty() &&
        std::ranges::find(oracle_case_ids,
                          test_case.native_oracle_case_id) ==
            oracle_case_ids.end())
      oracle_case_ids.push_back(test_case.native_oracle_case_id);
    if (!oracle_case_ids.empty())
      check_native_instruction(test_case, oracle_case_ids, location);
  }

  void check_native_instruction(
      const InstructionCase &test_case,
      const std::span<const std::string_view> oracle_case_ids,
      const std::source_location &location) {
    auto native_initial = test_case.initial;
    native_initial.pc = code_base;
    native_initial.access_fp_state();
    auto instruction_expected = test_case.expected;
    instruction_expected.pc =
        test_case.event == ExpectedEvent::system_call
            ? code_base + sizeof(std::uint32_t)
            : code_base;
    instruction_expected.access_fp_state();

    NativeMemoryImage native_memory{test_case.memory};
    const auto address_operands = native_address_operands(test_case);
    const auto &host =
        ::uml::platform::apple::probed_host_capabilities();
    const bool unavailable_state =
        native_requires_unavailable_feature_state(
            test_case.native.features()) ||
        native_unrepresented_state_changes(native_initial,
                                             instruction_expected) ||
        !native_fp_state_is_round_trippable(
            native_initial, instruction_expected, host) ||
        native_memory_has_tags(test_case.memory) ||
        address_operands.unsupported;
    native_oracle::Request request{
        .oracle_case_id = oracle_case_ids.front(),
        .instruction = test_case.instruction,
        .guest_pc = code_base,
        .initial = native_initial,
        .required_features = test_case.native.features(),
        .memory = native_memory.regions(),
        .invalid_encoding = test_case.invalid_encoding,
        .uses_platform_register =
            native_may_reference_platform_register(test_case.instruction),
        .requires_unavailable_system_state = unavailable_state,
        .requires_unsupported_event =
            !native_event_is_representable(test_case.event) ||
            native_exact_fault_event_is_unrepresentable(test_case),
        .requires_unavailable_memory_geometry =
            address_operands.unavailable_geometry ||
            native_literal_access(test_case.instruction),
        .requires_exact_memory_boundaries =
            test_case.event == ExpectedEvent::memory_fault &&
            test_case.compare_fault && !test_case.memory.empty(),
        .input_address_register_mask =
            address_operands.input_register_mask,
        .output_address_register_mask =
            address_operands.output_register_mask,
        .output_pc_relative_register_mask =
            address_operands.output_pc_relative_register_mask,
        .input_stack_pointer_is_address =
            address_operands.input_stack_pointer,
        .output_stack_pointer_is_address =
            address_operands.output_stack_pointer,
    };
    const auto observation = native_oracle::execute(request, host);

    if (observation.eligibility == native_oracle::Eligibility::eligible) {
      if (observation.status != native_oracle::ExecutionStatus::executed)
        fail(test_case.name, "native-hardware", test_case.instruction,
             "native evidence infrastructure failure");
      if (observation.event != native_expected_event(test_case.event)) {
        std::fprintf(stderr,
                     "expected native event %.*s, actual event %.*s\n",
                     static_cast<int>(native_oracle::name(
                         native_expected_event(test_case.event)).size()),
                     native_oracle::name(
                         native_expected_event(test_case.event)).data(),
                     static_cast<int>(native_oracle::name(
                         observation.event).size()),
                     native_oracle::name(observation.event).data());
        fail(test_case.name, "native-hardware", test_case.instruction,
             "unexpected instruction-boundary event");
      }
      if (observation.retired_instructions !=
          native_expected_retirement(test_case.event))
        fail(test_case.name, "native-hardware", test_case.instruction,
             "instruction-boundary retirement");

      auto compared_observed = observation.state;
      compared_observed.access_fp_state();
      if (test_case.validator != nullptr)
        test_case.validator(native_initial, instruction_expected,
                            compared_observed, test_case.name,
                            "native-hardware", test_case.instruction);
      else
        require_architecture_equal(
            instruction_expected, compared_observed, test_case.name,
            "native-hardware/instruction-boundary",
            test_case.instruction);
      if (test_case.compare_fault &&
          observation.fault_address != test_case.fault.address)
        fail(test_case.name, "native-hardware", test_case.instruction,
             "fault address");
      require_native_memory(test_case, observation);
    }

    for (const auto oracle_case_id : oracle_case_ids)
      native_oracle::record_observation(
          oracle_case_id, test_case.name, test_case.instruction,
          test_case.native.features(), host, request, observation, 1,
          location);
  }

  void record_native_program_scope(
      const ProgramCase &test_case,
      const std::span<const std::uint32_t> evidence_words,
      const std::source_location &location) {
    auto native_initial = test_case.initial;
    native_initial.pc = code_base + test_case.entry_offset;
    native_initial.access_fp_state();
    NativeMemoryImage native_memory{test_case.memory};
    const auto &host =
        ::uml::platform::apple::probed_host_capabilities();
    for (const auto instruction : evidence_words) {
      auto selected = native_oracle::select_native_allocations(
          instruction, native_oracle::SelectionScope::program, location);
      if (selected.allocation_status_ids.empty())
        continue;
      native_oracle::Request request{
          .oracle_case_id = selected.allocation_status_ids.front(),
          .instruction = instruction,
          .guest_pc = native_initial.pc,
          .initial = native_initial,
          .required_features = test_case.native.features(),
          .memory = native_memory.regions(),
          .invalid_encoding = test_case.invalid_encoding,
          .program_scope = true,
      };
      const auto observation = native_oracle::execute(request, host);
      if (observation.eligibility !=
              native_oracle::Eligibility::program_scope ||
          observation.status != native_oracle::ExecutionStatus::skipped)
        fail(test_case.name, "native-hardware", instruction,
             "program scope was not skipped deterministically");
      for (const auto &oracle_case_id : selected.allocation_status_ids)
        native_oracle::record_observation(
            oracle_case_id, test_case.name, instruction,
            test_case.native.features(), host, request, observation, 1,
            location);
    }
  }

  void check_program(const ProgramCase &test_case,
                     std::span<const std::uint32_t> evidence_words,
                     InstructionNativeScope native_scope,
                     const std::source_location &location) {
    if (test_case.expected_instructions.has_value() &&
        test_case.expected_instruction_pattern.has_value())
      fail(test_case.name, "setup", 0,
           "conflicting retired instruction expectations");
    if (test_case.expected_instruction_pattern.has_value() &&
        test_case.expected_instruction_pattern->step == 0)
      fail(test_case.name, "setup", 0,
           "zero retired instruction-count pattern step");
    if (test_case.expected_instruction_pattern.has_value() &&
        test_case.expected_instruction_pattern->maximum.has_value() &&
        *test_case.expected_instruction_pattern->maximum <
            test_case.expected_instruction_pattern->minimum)
      fail(test_case.name, "setup", 0,
           "invalid retired instruction-count pattern range");
    if (test_case.entry_offset % sizeof(std::uint32_t) != 0 ||
        test_case.entry_offset >
            DARWIN_UML_GUEST_PAGE_SIZE - sizeof(std::uint32_t))
      fail(test_case.name, "setup", 0, "invalid entry offset");
    std::uint32_t diagnostic_instruction = 0;
    bool found_entry = false;
    for (const auto word : test_case.code)
      if (word.offset == test_case.entry_offset) {
        diagnostic_instruction = word.instruction;
        found_entry = true;
        break;
      }
    if (!found_entry)
      fail(test_case.name, "setup", 0, "entry instruction is absent");

    validate_memory_regions(test_case.name, diagnostic_instruction,
                            test_case.memory);
    std::vector<std::unique_ptr<CaseRegion>> regions;
    regions.reserve(test_case.memory.size());
    for (const auto &region : test_case.memory)
      regions.push_back(std::make_unique<CaseRegion>(generation_, region));

    std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> expected_page{};
    install_program(test_case.name, diagnostic_instruction, test_case.code,
                    expected_page, installed_page_);

    auto initial = test_case.initial;
    initial.pc = code_base + test_case.entry_offset;
    auto digest_initial = initial;
    digest_initial.access_fp_state();
    const auto initial_state_digest =
        instruction_evidence::architecture_state_digest(digest_initial);
    const auto initial_memory_digest =
        memory_initial_digest(test_case.memory);

    auto expected = test_case.expected;
    auto digest_expected = expected;
    digest_expected.access_fp_state();
    const auto expected_state_digest =
        instruction_evidence::architecture_state_digest(digest_expected);
    const auto expected_memory_digest =
        memory_expectation_digest(test_case.memory);
    const auto expected_retirement = retirement_expectation(test_case);
    const auto expected_fault = fault_expectation(test_case);
    const auto expected_state_oracle =
        state_oracle_identity(test_case, location);
    std::optional<std::size_t> interpreter_instructions;
    InstructionEngineEvidence engine_evidence;

    for (const auto engine :
         {Engine::interpreter, Engine::gadgets, Engine::jit}) {
      if (!::uml::cpu::cpu_engine_available(engine)) {
        if (engine != Engine::jit)
          fail(test_case.name, "setup", diagnostic_instruction,
               "required CPU engine is unavailable");
        ++counts_.jit_skipped;
        ++counts_.jit_unavailable;
        engine_evidence.jit = InstructionEngineResult::unavailable;
        continue;
      }
      if (engine == Engine::jit && !test_case.invalid_encoding &&
          !host_supports(test_case.native)) {
        ++counts_.jit_skipped;
        ++counts_.jit_skipped_host_feature;
        engine_evidence.jit =
            InstructionEngineResult::skipped_host_feature;
        continue;
      }

      for (const auto &region : regions)
        region->restore();
      void *translated = nullptr;
      if (darwin_uml_static_vm_translate(
              generation_, code_base, DARWIN_UML_STATIC_EXECUTE, &translated) !=
              DARWIN_UML_STATIC_TRANSLATED ||
          translated == nullptr)
        fail(test_case.name, engine_name(engine), diagnostic_instruction,
             "code mapping disappeared");
      StaticGuestMemory memory;
      const auto sequences = darwin_uml_static_vm_sequences(generation_);
      memory.prepare(generation_, sequences.mapping_changes);
      const auto current_engine = engine_name(engine);
      if (test_case.before_engine != nullptr)
        test_case.before_engine(
            test_case.execution_context, current_engine, generation_);
      Outcome outcome;
      if (engine == Engine::gadgets && forced_gadget_executor_ != nullptr) {
        outcome = run_forced_gadget_executor(
            initial, memory, generation_, *forced_gadget_executor_);
      } else {
        auto *selected_engine = engine_instance(engine);
        if (selected_engine == nullptr)
          fail(test_case.name, "setup", diagnostic_instruction,
               "selected CPU engine was not constructed");
        outcome = run_engine(engine, initial, memory, generation_,
                             *selected_engine);
      }
      const auto evidence_result = outcome.instructions == 0
          ? InstructionEngineResult::executed_not_retired
          : InstructionEngineResult::executed_retired;
      switch (engine) {
      case Engine::interpreter:
        ++counts_.interpreter;
        engine_evidence.interpreter = evidence_result;
        break;
      case Engine::gadgets:
        ++counts_.gadgets;
        engine_evidence.gadgets = evidence_result;
        break;
      case Engine::jit:
        ++counts_.jit;
        engine_evidence.jit = evidence_result;
        break;
      }
      if (test_case.after_engine != nullptr)
        test_case.after_engine(
            test_case.execution_context, current_engine, generation_);

      /* The public engine boundary owns translated host-state restoration and
       * executor cleanup. Do not repair leaked state in this common harness:
       * doing so would let the full instruction matrix pass while a direct
       * production caller observes an active guest FP environment or a stale
       * indirect-link marker. */
      if (outcome.cpu.gadget_fp_active ||
          outcome.cpu.gadget_fpmr_active ||
          outcome.cpu.gadget_indirect_miss != nullptr)
        fail(test_case.name, current_engine, diagnostic_instruction,
             "engine-private execution state escaped the public boundary");

      if (outcome.event != test_case.event) {
        std::fprintf(stderr,
                     "expected event %.*s, actual event %.*s, "
                     "instructions=%zu fault=%#llx/%u\n",
                     static_cast<int>(event_name(test_case.event).size()),
                     event_name(test_case.event).data(),
                     static_cast<int>(event_name(outcome.event).size()),
                     event_name(outcome.event).data(), outcome.instructions,
                     static_cast<unsigned long long>(outcome.fault.address),
                     static_cast<unsigned>(outcome.fault.access));
        fail(test_case.name, current_engine, diagnostic_instruction,
             "unexpected terminal event");
      }

      if (test_case.expected_instructions.has_value()) {
        if (outcome.instructions != *test_case.expected_instructions) {
          std::fprintf(stderr,
                       "expected instructions=%zu actual instructions=%zu\n",
                       *test_case.expected_instructions,
                       outcome.instructions);
          fail(test_case.name, current_engine, diagnostic_instruction,
               "retired instruction count");
        }
      } else if (test_case.expected_instruction_pattern.has_value()) {
        const auto &pattern = *test_case.expected_instruction_pattern;
        const bool below_minimum = outcome.instructions < pattern.minimum;
        const bool above_maximum =
            pattern.maximum.has_value() &&
            outcome.instructions > *pattern.maximum;
        const bool wrong_step =
            !below_minimum &&
            (outcome.instructions - pattern.minimum) % pattern.step != 0;
        if (below_minimum || above_maximum || wrong_step) {
          std::fprintf(
              stderr,
              "expected instructions=%zu+N*%zu maximum=%zu actual=%zu\n",
              pattern.minimum, pattern.step,
              pattern.maximum.value_or(std::numeric_limits<std::size_t>::max()),
              outcome.instructions);
          fail(test_case.name, current_engine, diagnostic_instruction,
               "retired instruction-count pattern");
        }
      } else if (test_case.event != ExpectedEvent::instruction_budget &&
                 engine == Engine::interpreter) {
        interpreter_instructions = outcome.instructions;
      } else if (test_case.event != ExpectedEvent::instruction_budget &&
                 interpreter_instructions.has_value() &&
                 outcome.instructions != *interpreter_instructions) {
        std::fprintf(stderr,
                     "interpreter instructions=%zu actual instructions=%zu\n",
                     *interpreter_instructions, outcome.instructions);
        fail(test_case.name, current_engine, diagnostic_instruction,
             "engine instruction-count mismatch");
      }

      /*
       * Deferred state is part of the engine-entry contract. Do not
       * materialize it before execution; normalize copies only after the
       * engine has finished so architecture comparisons remain bit-exact.
       */
      auto compared_initial = initial;
      auto compared_expected = expected;
      auto compared_actual = outcome.cpu;
      compared_initial.access_fp_state();
      compared_expected.access_fp_state();
      compared_actual.access_fp_state();
      if (test_case.validator != nullptr)
        test_case.validator(compared_initial, compared_expected,
                            compared_actual, test_case.name, current_engine,
                            diagnostic_instruction);
      else
        require_architecture_equal(compared_expected, compared_actual,
                                   test_case.name, current_engine,
                                   diagnostic_instruction);
      if (test_case.compare_fault &&
          (outcome.fault.address != test_case.fault.address ||
           outcome.fault.access != test_case.fault.access)) {
        std::fprintf(
            stderr,
            "expected fault=%#llx/%u actual fault=%#llx/%u\n",
            static_cast<unsigned long long>(test_case.fault.address),
            static_cast<unsigned>(test_case.fault.access),
            static_cast<unsigned long long>(outcome.fault.address),
            static_cast<unsigned>(outcome.fault.access));
        fail(test_case.name, current_engine, diagnostic_instruction,
             "fault metadata");
      }
      if (test_case.event == ExpectedEvent::floating_point_trap &&
          outcome.syndrome != test_case.syndrome) {
        std::fprintf(stderr, "expected syndrome=%#x actual=%#x\n",
                     test_case.syndrome, outcome.syndrome);
        fail(test_case.name, current_engine, diagnostic_instruction,
             "floating-point syndrome");
      }

      if (std::memcmp(translated, expected_page.data(),
                      expected_page.size()) != 0)
        fail(test_case.name, current_engine, diagnostic_instruction,
             "unexpected code-page write");
      for (const auto &region : regions)
        region->require_expected(test_case.name, current_engine,
                                 diagnostic_instruction);
    }
    record_instruction_input_materialization(
        {.case_name = test_case.name,
         .code = test_case.code,
         .entry_offset = test_case.entry_offset,
         .evidence_words = evidence_words,
         .installed_code_page = installed_page_,
         .initial = digest_initial,
         .expected = digest_expected,
         .memory = test_case.memory,
         .expected_event = event_name(test_case.event),
         .expected_retirement = expected_retirement,
         .instruction_budget = execution_limit,
         .expected_state_oracle = expected_state_oracle,
         .expected_fault = expected_fault,
         .expected_syndrome = test_case.syndrome,
         .native = test_case.native,
         .native_scope = native_scope,
         .invalid_encoding = test_case.invalid_encoding,
         .has_custom_validator = test_case.validator != nullptr,
         .has_before_engine_hook = test_case.before_engine != nullptr,
         .has_after_engine_hook = test_case.after_engine != nullptr,
         .validator_relation_id = test_case.validator_relation_id,
         .before_engine_effect_id = test_case.before_engine_effect_id,
         .after_engine_effect_id = test_case.after_engine_effect_id,
         .initial_state_digest = initial_state_digest,
         .initial_memory_digest = initial_memory_digest,
         .expected_state_digest = expected_state_digest,
         .expected_memory_digest = expected_memory_digest,
         .engines = engine_evidence,
         .native_observation = {
             .required = test_case.native.features(),
             .observed = ::uml::platform::apple::probed_host_capabilities()
                             .supported}},
        location);
    for (const auto instruction : evidence_words)
      record_instruction_evidence(test_case.name, instruction,
                                  test_case.invalid_encoding, engine_evidence,
                                  {.required = test_case.native.features(),
                                   .observed = ::uml::platform::apple::
                                       probed_host_capabilities().supported},
                                  native_scope,
                                  {.event = event_name(test_case.event),
                                   .retirement = expected_retirement,
                                   .state_oracle = expected_state_oracle,
                                   .initial_state_digest = initial_state_digest,
                                   .initial_memory_digest =
                                       initial_memory_digest,
                                   .state_digest = expected_state_digest,
                                   .memory_digest = expected_memory_digest,
                                   .fault = expected_fault,
                                   .syndrome = test_case.syndrome},
                                  location);
    if (native_scope == InstructionNativeScope::program)
      record_native_program_scope(test_case, evidence_words, location);
  }

  [[nodiscard]] const EngineTestCounts &counts() const noexcept {
    return counts_;
  }

private:
  [[nodiscard]] ::uml::cpu::CpuEngine *
  engine_instance(Engine engine) noexcept {
    switch (engine) {
    case Engine::interpreter:
      return interpreter_engine_.get();
    case Engine::gadgets:
      return gadget_engine_.get();
    case Engine::jit:
      return jit_engine_.get();
    }
    return nullptr;
  }

  unsigned long generation_;
  AddressSpace address_space_;
  Backing backing_;
  Mapping mapping_;
  std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> installed_page_{};
  std::unique_ptr<::uml::cpu::CpuEngine> interpreter_engine_;
  std::unique_ptr<::uml::cpu::CpuEngine> gadget_engine_;
  std::unique_ptr<::uml::cpu::CpuEngine> jit_engine_;
  std::unique_ptr<::uml::cpu::arm64::gadgets::GadgetExecutor>
      forced_gadget_executor_;
  EngineTestCounts counts_;
};

EngineHarness::EngineHarness(EngineHarnessOptions options)
    : implementation_{std::make_unique<Implementation>(options)} {}

EngineHarness::~EngineHarness() = default;

void EngineHarness::check(const InstructionCase &test_case,
                          const std::source_location location) {
  implementation_->check(test_case, location);
}

void EngineHarness::check_program(const ProgramCase &test_case,
                                  const std::source_location location) {
  /* The declared entry word is necessarily attempted by every engine. A
   * terminal result and aggregate retirement count do not prove that any
   * other word in a branching program was reached, so only the entry may
   * contribute inventory evidence without an explicit instruction trace. */
  const auto entry = std::find_if(
      test_case.code.begin(), test_case.code.end(), [&](const ProgramWord word) {
        return word.offset == test_case.entry_offset;
      });
  if (entry == test_case.code.end()) {
    implementation_->check_program(
        test_case, {}, InstructionNativeScope::program, location);
    return;
  }
  const std::array evidence_words{entry->instruction};
  implementation_->check_program(
      test_case, evidence_words, InstructionNativeScope::program, location);
}

void EngineHarness::check_one(std::string_view name, std::uint32_t instruction,
                              const arm64::CpuState &initial,
                              const arm64::CpuState &expected,
                              NativeRequirements native,
                              const std::source_location location) {
  check({
      .name = name,
      .instruction = instruction,
      .initial = initial,
      .expected = expected,
      .native = native,
  }, location);
}

void EngineHarness::check_illegal(std::string_view name,
                                  std::uint32_t instruction,
                                  const arm64::CpuState &initial,
                                  NativeRequirements native,
                                  const std::source_location location) {
  check({
      .name = name,
      .instruction = instruction,
      .initial = initial,
      .expected = initial,
      .event = ExpectedEvent::illegal_instruction,
      .native = native,
      .invalid_encoding = true,
  }, location);
}

const EngineTestCounts &EngineHarness::counts() const noexcept {
  return implementation_->counts();
}

void EngineHarness::print_summary(std::string_view suite) const {
  const auto &value = counts();
  std::printf("%.*s: interpreter=%zu gadgets=%zu jit=%zu "
              "jit-skipped-by-host-feature=%zu jit-unavailable=%zu\n",
              static_cast<int>(suite.size()), suite.data(), value.interpreter,
              value.gadgets, value.jit, value.jit_skipped_host_feature,
              value.jit_unavailable);
}

} // namespace uml::emulator::tests
