// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"

#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "runtime/host_features.hpp"
#include "runtime/jit/jit_executor.hpp"
#include "runtime/static/gadget_executor.hpp"
#include "runtime/static/guest_memory.hpp"

#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace uml::backend::tests {
namespace {

constexpr std::uint64_t code_base = engine_test_code_base;
constexpr std::size_t execution_limit = 64;
constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

enum class Engine {
  interpreter,
  gadgets,
  jit,
};

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

[[nodiscard]] constexpr ExpectedEvent
event_for(arm64::StopReason reason) noexcept {
  switch (reason) {
  case arm64::StopReason::system_call:
    return ExpectedEvent::system_call;
  case arm64::StopReason::memory_fault:
    return ExpectedEvent::memory_fault;
  case arm64::StopReason::gcs_fault:
    return ExpectedEvent::gcs_fault;
  case arm64::StopReason::illegal_instruction:
    return ExpectedEvent::illegal_instruction;
  case arm64::StopReason::breakpoint:
    return ExpectedEvent::breakpoint;
  case arm64::StopReason::floating_point_trap:
    return ExpectedEvent::floating_point_trap;
  case arm64::StopReason::budget_exhausted:
    return ExpectedEvent::instruction_budget;
  case arm64::StopReason::unsupported_instruction:
  case arm64::StopReason::continued:
    return ExpectedEvent::fatal;
  }
  return ExpectedEvent::fatal;
}

[[nodiscard]] constexpr ExpectedEvent
event_for(arm64::gadgets::Event event) noexcept {
  switch (event) {
  case arm64::gadgets::Event::system_call:
    return ExpectedEvent::system_call;
  case arm64::gadgets::Event::memory_fault:
    return ExpectedEvent::memory_fault;
  case arm64::gadgets::Event::gcs_fault:
    return ExpectedEvent::gcs_fault;
  case arm64::gadgets::Event::illegal_instruction:
    return ExpectedEvent::illegal_instruction;
  case arm64::gadgets::Event::breakpoint:
    return ExpectedEvent::breakpoint;
  case arm64::gadgets::Event::floating_point_trap:
    return ExpectedEvent::floating_point_trap;
  case arm64::gadgets::Event::interrupt:
    return ExpectedEvent::instruction_budget;
  case arm64::gadgets::Event::fatal:
  case arm64::gadgets::Event::continued:
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

[[nodiscard]] Outcome run_interpreter(const arm64::CpuState &initial,
                                      StaticGuestMemory &memory,
                                      unsigned long generation) {
  Outcome outcome{};
  outcome.cpu = initial;
  ExecutionScope scope{generation, outcome.cpu.pc};
  if (!scope.entered())
    return outcome;
  const auto sequences = darwin_uml_static_vm_sequences(generation);
  memory.prepare(generation, sequences.mapping_changes);
  const auto result =
      arm64::Dispatcher{}.run(outcome.cpu, memory, execution_limit);
  outcome.event = event_for(result.reason);
  outcome.instructions = result.instructions;
  outcome.syndrome = result.syndrome;
  if (result.reason == arm64::StopReason::memory_fault) {
    outcome.fault = memory.last_fault();
    outcome.fault.access =
        result.write ? arm64::MemoryAccess::write : arm64::MemoryAccess::read;
    /*
     * Alignment and other architecturally generated data faults occur before
     * GuestMemory is called. Preserve the dispatcher's fault metadata for
     * those cases. Otherwise retain GuestMemory's exact failing-page address
     * for accesses that cross a mapping boundary, while taking the
     * architectural access direction from the dispatch result rather than
     * from an implementation-internal probe.
     */
    if (!memory.has_fault())
      outcome.fault = {
          result.address,
          result.write ? arm64::MemoryAccess::write
                       : arm64::MemoryAccess::read,
      };
  }
  return outcome;
}

template <typename Executor>
[[nodiscard]] Outcome
run_translated(const arm64::CpuState &initial, StaticGuestMemory &memory,
               unsigned long generation, Executor &executor) {
  Outcome outcome{};
  outcome.cpu = initial;
  if (!executor.begin_execution(generation, outcome.cpu.pc))
    return outcome;
  for (std::size_t blocks = 0; blocks < execution_limit; ++blocks) {
    std::size_t block_instructions = 0;
    const auto event =
        executor.run_block(outcome.cpu, memory, generation, block_instructions);
    outcome.instructions += block_instructions;
    if (event == arm64::gadgets::Event::continued && !executor.preempted())
      continue;
    outcome.event =
        event == arm64::gadgets::Event::continued && executor.preempted()
            ? ExpectedEvent::instruction_budget
            : event_for(event);
    if (event == arm64::gadgets::Event::memory_fault)
      outcome.fault = {executor.fault_address(),
                       executor.fault_access() == arm64::gadgets::Access::write
                           ? arm64::MemoryAccess::write
                           : arm64::MemoryAccess::read};
    if (event == arm64::gadgets::Event::floating_point_trap)
      outcome.syndrome = executor.floating_point_syndrome();
    executor.end_execution();
    arm64::gadgets::restore_native_fp_environment(outcome.cpu);
    return outcome;
  }
  executor.end_execution();
  arm64::gadgets::restore_native_fp_environment(outcome.cpu);
  outcome.event = ExpectedEvent::instruction_budget;
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
  return host_arm_features().supports(requirements.features());
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
                 false},
        gadget_executor_{arm64::gadgets::BuildOptions{
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
        }},
        jit_executor_{JitExecutorOptions{
            .enable_guarded_control_flow =
                options.enable_guarded_control_flow,
            .enable_pointer_authentication =
                options.enable_pointer_authentication,
        }} {}

  void check(const InstructionCase &test_case) {
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
        .execution_context = test_case.execution_context,
        .before_engine = test_case.before_engine,
        .after_engine = test_case.after_engine,
        .expected_instructions = instruction_count_expectation(test_case),
    });
  }

  void check_program(const ProgramCase &test_case) {
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

    auto expected = test_case.expected;
    std::optional<std::size_t> interpreter_instructions;

    for (const auto engine :
         {Engine::interpreter, Engine::gadgets, Engine::jit}) {
      if (engine == Engine::jit && !native_jit_available()) {
        ++counts_.jit_skipped;
        ++counts_.jit_unavailable;
        continue;
      }
      if (engine == Engine::jit && !test_case.invalid_encoding &&
          !host_supports(test_case.native)) {
        ++counts_.jit_skipped;
        ++counts_.jit_skipped_host_feature;
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
      switch (engine) {
      case Engine::interpreter:
        outcome = run_interpreter(initial, memory, generation_);
        ++counts_.interpreter;
        break;
      case Engine::gadgets: {
        outcome =
            run_translated(initial, memory, generation_, gadget_executor_);
        ++counts_.gadgets;
        break;
      }
      case Engine::jit: {
        outcome = run_translated(initial, memory, generation_, jit_executor_);
        ++counts_.jit;
        break;
      }
      }
      if (test_case.after_engine != nullptr)
        test_case.after_engine(
            test_case.execution_context, current_engine, generation_);

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
  }

  [[nodiscard]] const EngineTestCounts &counts() const noexcept {
    return counts_;
  }

private:
  unsigned long generation_;
  AddressSpace address_space_;
  Backing backing_;
  Mapping mapping_;
  std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> installed_page_{};
  GadgetExecutor gadget_executor_;
  JitExecutor jit_executor_;
  EngineTestCounts counts_;
};

EngineHarness::EngineHarness(EngineHarnessOptions options)
    : implementation_{std::make_unique<Implementation>(options)} {}

EngineHarness::~EngineHarness() = default;

void EngineHarness::check(const InstructionCase &test_case) {
  implementation_->check(test_case);
}

void EngineHarness::check_program(const ProgramCase &test_case) {
  implementation_->check_program(test_case);
}

void EngineHarness::check_one(std::string_view name, std::uint32_t instruction,
                              const arm64::CpuState &initial,
                              const arm64::CpuState &expected,
                              NativeRequirements native) {
  check({
      .name = name,
      .instruction = instruction,
      .initial = initial,
      .expected = expected,
      .native = native,
  });
}

void EngineHarness::check_illegal(std::string_view name,
                                  std::uint32_t instruction,
                                  const arm64::CpuState &initial,
                                  NativeRequirements native) {
  check({
      .name = name,
      .instruction = instruction,
      .initial = initial,
      .expected = initial,
      .event = ExpectedEvent::illegal_instruction,
      .native = native,
      .invalid_encoding = true,
  });
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

} // namespace uml::backend::tests
