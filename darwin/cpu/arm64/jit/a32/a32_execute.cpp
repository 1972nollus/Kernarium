// SPDX-License-Identifier: GPL-2.0-or-later
//
// The terminal slow path of the AArch32 translation (a32_helpers.hpp): one
// instruction the inline lowering declines, executed on the register file the
// generated code spilled. Everything but the exclusives runs in the AArch32
// semantic library (cpu/arm64/aarch32/semantics.hpp) over the Tlb, with the
// library's fault ordering: a page crossing, a misalignment the architecture
// faults on, a faulting access and the forms the compiler leaves to it.
//
// The exclusives stay here: the library's monitor is a compare-and-swap of
// the recorded value, while the JIT's guest monitor is the versioned
// software reservation table that its tracked stores and its fused host
// exclusives share (jit_semantic_reservation.hpp).

#include "a32_helpers.hpp"

#include "a32_operations.hpp"
#include "a32_transfer.hpp"

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/semantics.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/jit/jit_publication.hpp"
#include "cpu/arm64/jit/jit_runtime_contract.hpp"
#include "cpu/arm64/jit/jit_semantic_reservation.hpp"
#include "cpu/arm64/memory.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>
#include <span>

namespace uml::cpu::arm64 {

namespace {

using ::uml::memory::Access;
using ::uml::memory::Tlb;
namespace a32 = jit::a32;
namespace sem = jit::semantic;
using a32::Transfer;
using aarch32::Instruction;
using aarch32::SRType;

/* The executor's view of guest memory for one instruction: the Tlb's
 * translations, with the failing page recorded in the Tlb's fault fields as
 * the generated code's own translations record it. A copy of the A64
 * semantic path's view (jit_semantic_memory.cpp), which is private to that
 * file. */
class TlbMemory final : public GuestMemory {
public:
  explicit TlbMemory(Tlb &tlb) noexcept : tlb_{tlb} {}

  bool read(std::uint64_t address, std::span<std::byte> destination) override {
    while (!destination.empty()) {
      auto *source = tlb_.translate(address, Access::read);
      if (source == nullptr)
        return false;
      const auto count = std::min(
          destination.size(),
          static_cast<std::size_t>(page_size - (address & (page_size - 1U))));
      std::memcpy(destination.data(), source, count);
      address += count;
      destination = destination.subspan(count);
    }
    return true;
  }

  bool write(std::uint64_t address,
             std::span<const std::byte> source) override {
    /* A store is one access: translate every page it touches before the first
     * host store, so a fault on a later page exposes no partial update. A
     * transfer spans at most two pages. */
    std::array<std::byte *, 2> destinations{};
    std::array<std::size_t, 2> lengths{};
    std::size_t pages = 0;
    std::size_t completed = 0;
    while (completed != source.size()) {
      if (pages == destinations.size())
        return false;
      const auto current = address + completed;
      tlb_.discard(current);
      auto *destination = tlb_.translate(current, Access::write);
      if (destination == nullptr)
        return false;
      const auto count = std::min(
          source.size() - completed,
          static_cast<std::size_t>(page_size - (current & (page_size - 1U))));
      destinations[pages] = destination;
      lengths[pages] = count;
      ++pages;
      completed += count;
    }
    completed = 0;
    for (std::size_t page = 0; page < pages; ++page) {
      std::memcpy(destinations[page], source.data() + completed, lengths[page]);
      completed += lengths[page];
    }
    return true;
  }

  bool validate_access(std::uint64_t address, std::size_t size,
                       ::uml::memory::MemoryAccess access) override {
    const auto direction = access == ::uml::memory::MemoryAccess::write
                               ? Access::write
                               : Access::read;
    std::size_t completed = 0;
    while (completed < size) {
      const auto current = address + completed;
      if (tlb_.translate(current, direction) == nullptr)
        return false;
      completed += std::min(
          size - completed,
          static_cast<std::size_t>(page_size - (current & (page_size - 1U))));
    }
    return true;
  }

  /* One host access on the translated page, for the exclusives. The caller
   * has checked natural alignment, so the access never crosses a page. The
   * dynamic loader can change COW permissions between accesses, so a write
   * translation is revalidated here. */
  std::byte *atomic_pointer(std::uint64_t address, std::size_t size,
                            ::uml::memory::MemoryAccess access) override {
    if (size == 0 || size > 8U || (size & (size - 1U)) != 0 ||
        (address & (size - 1U)) != 0)
      return nullptr;
    const auto direction = ::uml::memory::translated_access(access);
    if (direction == Access::write)
      tlb_.discard(address);
    return tlb_.translate(address, direction);
  }

  std::byte *reservation_pointer(std::uint64_t address, std::size_t size,
                                 ::uml::memory::MemoryAccess access) override {
    if (size == 0 || size > page_size - (address & (page_size - 1U)))
      return nullptr;
    return tlb_.translate(address, ::uml::memory::translated_access(access));
  }

  [[nodiscard]] ::uml::memory::MemoryFault
  last_fault() const noexcept override {
    return {tlb_.fault_address, ::uml::memory::guest_access(tlb_.fault_access)};
  }

  [[nodiscard]] bool has_fault() const noexcept override { return true; }

private:
  Tlb &tlb_;
};


/* The library's view of guest memory: the Tlb's translations, with a write
 * translation revalidated (the dynamic loader can change COW permissions
 * between accesses). The failing page is the library's to report. */
class LibraryMemory final : public aarch32::A32Memory {
public:
  explicit LibraryMemory(Tlb &tlb) noexcept : tlb_{tlb} {}

  std::byte *translate(std::uint64_t address,
                       ::uml::memory::MemoryAccess access) noexcept override {
    const auto direction = ::uml::memory::translated_access(access);
    if (direction == Access::write)
      tlb_.discard(address);
    return tlb_.translate(address, direction);
  }

  /* An ordinary store invalidates the reservation granules it touched, as
   * the native store blocks do when their publication ends. */
  void stored(std::byte *host, std::size_t size) noexcept override {
    const JitStoreRange range{reinterpret_cast<std::uint64_t>(host),
                                   size};
    sem::invalidate_store_ranges(std::span{&range, 1});
  }

private:
  Tlb &tlb_;
};

enum class Outcome : std::uint8_t {
  retired,
  /* A translation failed: the Tlb holds the address and access. */
  fault,
  /* The access was misaligned for its class. */
  alignment,
  illegal,
};

/* A load-exclusive or store-exclusive on the JIT's software reservation. */
class Exclusive final {
public:
  Exclusive(CpuState &cpu, Tlb &tlb, const Instruction &inst,
            std::uint64_t key, const Transfer &transfer)
      : cpu_{cpu}, tlb_{tlb}, memory_{tlb}, inst_{inst}, transfer_{transfer},
        thumb_{aarch32::key_is_thumb(key)}, pc_{aarch32::key_pc(key)} {}

  [[nodiscard]] Outcome run() { return exclusive(); }

  [[nodiscard]] std::uint64_t fault_address() const noexcept {
    return fault_address_;
  }
  [[nodiscard]] bool fault_is_write() const noexcept { return fault_write_; }

private:
  /* The value a read of register r sees. */
  [[nodiscard]] std::uint32_t read(unsigned r) const noexcept {
    return r == 15U ? pc_ + (thumb_ ? 4U : 8U)
                    : static_cast<std::uint32_t>(cpu_.registers[r]);
  }
  void write(unsigned r, std::uint32_t value) noexcept {
    cpu_.registers[r] = value;
  }

  /* The instruction retired and execution continues after it. */
  [[nodiscard]] Outcome retire() noexcept {
    cpu_.pc = thumb_ ? aarch32::t32_key(pc_ + inst_.length)
                     : aarch32::a32_key(pc_ + inst_.length);
    return Outcome::retired;
  }

  [[nodiscard]] Outcome alignment_fault(std::uint32_t address, bool write) {
    fault_address_ = address;
    fault_write_ = write;
    return Outcome::alignment;
  }
  [[nodiscard]] Outcome memory_fault(bool write) {
    fault_address_ = tlb_.fault_address;
    fault_write_ = write;
    return Outcome::fault;
  }

  [[nodiscard]] Outcome exclusive() {
    const std::uint32_t address = read(transfer_.n) + transfer_.imm;
    const unsigned bytes = transfer_.bytes;
    if ((address & (bytes - 1U)) != 0U)
      return alignment_fault(address, !transfer_.load);
    if (transfer_.load) {
      std::scoped_lock lock{sem::exclusive_memory_mutex};
      std::uint64_t value = 0;
      std::uintptr_t key = 0;
      if (!sem::atomic_load_value(memory_, address, bytes,
                                  transfer_.acquire
                                      ? std::memory_order_acquire
                                      : std::memory_order_relaxed,
                                  value, key))
        return memory_fault(false);
      write(transfer_.t, static_cast<std::uint32_t>(value));
      if (bytes == 8U)
        write(transfer_.t2, static_cast<std::uint32_t>(value >> 32));
      cpu_.exclusive_address = address;
      cpu_.exclusive_key = key;
      cpu_.exclusive_value = value;
      cpu_.exclusive_value2 = 0;
      cpu_.exclusive_version = sem::reservation_version(key);
      cpu_.exclusive_size = static_cast<std::uint8_t>(bytes);
      return retire();
    }
    std::uint64_t value = read(transfer_.t);
    if (bytes == 8U)
      value |= std::uint64_t{read(transfer_.t2)} << 32;
    else if (bytes == 1U)
      value &= 0xffU;
    else if (bytes == 2U)
      value &= 0xffffU;
    std::scoped_lock lock{sem::exclusive_memory_mutex};
    std::uint64_t current = 0;
    std::uintptr_t key = 0;
    if (!sem::atomic_load_value(memory_, address, bytes,
                                std::memory_order_relaxed, current, key))
      return memory_fault(true);
    bool success = cpu_.exclusive_address == address &&
                   cpu_.exclusive_size == bytes && cpu_.exclusive_key == key &&
                   cpu_.exclusive_version == sem::reservation_version(key) &&
                   cpu_.exclusive_value == current;
    cpu_.exclusive_address = ~std::uint64_t{0};
    cpu_.exclusive_key = 0;
    cpu_.exclusive_value = 0;
    cpu_.exclusive_value2 = 0;
    cpu_.exclusive_version = 0;
    cpu_.exclusive_size = 0;
    if (success) {
      std::uintptr_t stored_key = 0;
      const auto result = sem::atomic_store_value(
          memory_, address, bytes, value,
          transfer_.release ? std::memory_order_release
                            : std::memory_order_relaxed,
          &key, stored_key);
      if (result == sem::AtomicStoreResult::fault)
        return memory_fault(true);
      if (result == sem::AtomicStoreResult::stored)
        sem::reservation_store(stored_key);
      else
        success = false;
    }
    write(transfer_.d, success ? 0U : 1U);
    return retire();
  }

  CpuState &cpu_;
  Tlb &tlb_;
  TlbMemory memory_;
  const Instruction &inst_;
  const Transfer &transfer_;
  bool thumb_;
  std::uint32_t pc_;
  std::uint64_t fault_address_{};
  bool fault_write_{};
};

[[nodiscard]] std::uint64_t finish(CpuState &cpu, Tlb &tlb,
                                   const Instruction &inst,
                                   std::uint32_t itstate, Outcome outcome,
                                   std::uint64_t fault_address,
                                   bool fault_write) noexcept {
  switch (outcome) {
  case Outcome::retired:
    cpu.a32_itstate = inst.next_it.bits();
    return pack_jit_result(Event::continued, 1);
  case Outcome::fault:
  case Outcome::alignment:
    /* The instruction did not retire: the task resumes at it, in the IT
     * state it was entered in. */
    cpu.a32_itstate = static_cast<std::uint8_t>(itstate);
    tlb.fault_address = fault_address;
    tlb.fault_access = fault_write ? Access::write : Access::read;
    tlb.fault_alignment = outcome == Outcome::alignment ? 1U : 0U;
    return pack_jit_result(Event::memory_fault, 0);
  case Outcome::illegal:
    break;
  }
  cpu.a32_itstate = static_cast<std::uint8_t>(itstate);
  return pack_jit_result(Event::illegal_instruction, 0);
}

[[nodiscard]] std::uint64_t execute_exclusive(CpuState &cpu, Tlb &tlb,
                                              const Instruction &inst,
                                              std::uint64_t key,
                                              std::uint32_t itstate,
                                              const Transfer &transfer) noexcept {
  Exclusive execution{cpu, tlb, inst, key, transfer};
  const auto outcome = execution.run();
  return finish(cpu, tlb, inst, itstate, outcome, execution.fault_address(),
                execution.fault_is_write());
}

/* The library's answer, as the JIT result the generated code returns. */
[[nodiscard]] std::uint64_t execute_in_library(CpuState &cpu, Tlb &tlb,
                                               const Instruction &inst,
                                               std::uint64_t key,
                                               std::uint32_t itstate) noexcept {
  cpu.pc = key;
  cpu.a32_itstate = static_cast<std::uint8_t>(itstate);
  LibraryMemory memory{tlb};
  aarch32::StepResult result;
  if ((inst.info().traits & aarch32::trait::stores) != 0U) {
    /* A store serializes with the exclusive operations, which check the
     * monitor and store under the same lock. */
    std::scoped_lock lock{sem::exclusive_memory_mutex};
    result = aarch32::execute_a32(cpu, inst, memory);
  } else {
    result = aarch32::execute_a32(cpu, inst, memory);
  }
  switch (result.event) {
  case aarch32::StepEvent::retired:
    return pack_jit_result(Event::continued, 1);
  case aarch32::StepEvent::memory_fault:
    tlb.fault_address = result.fault_address;
    tlb.fault_access = ::uml::memory::translated_access(result.access);
    tlb.fault_alignment = result.alignment ? 1U : 0U;
    return pack_jit_result(Event::memory_fault, 0);
  case aarch32::StepEvent::undefined:
    return pack_jit_result(Event::illegal_instruction, 0);
  default:
    break;
  }
  /* The compiler routes only what the library implements here. */
  return pack_jit_result(Event::fatal, 0);
}

std::atomic<bool> counting_exits{false};
std::atomic<std::uint64_t> exit_total{0};
std::array<std::atomic<std::uint64_t>, aarch32::kFormCount> form_exits{};

} // namespace

namespace jit::a32 {

void count_helper_exits(bool enable) noexcept {
  counting_exits.store(enable, std::memory_order_relaxed);
}

std::uint64_t helper_exits(aarch32::Form form) noexcept {
  return form_exits[static_cast<std::size_t>(form)].load(
      std::memory_order_relaxed);
}

std::uint64_t helper_exit_total() noexcept {
  return exit_total.load(std::memory_order_relaxed);
}

void reset_helper_exits() noexcept {
  exit_total.store(0, std::memory_order_relaxed);
  for (auto &count : form_exits)
    count.store(0, std::memory_order_relaxed);
}

} // namespace jit::a32

extern "C" std::uint64_t uml_arm64_jit_a32_execute(
    CpuState *cpu, Tlb *tlb, std::uint32_t word, std::uint64_t key,
    std::uint32_t itstate) noexcept {
  if (cpu == nullptr || tlb == nullptr || !aarch32::key_is_aarch32(key))
    return pack_jit_result(Event::fatal, 0);
  /* A terminal helper runs outside the native register environment. */
  arm64::restore_native_fp_environment(*cpu);
  const auto profile = aarch32::Profile::at(a32::a32_level());
  const auto inst =
      aarch32::key_is_thumb(key)
          ? aarch32::decode_thumb(
                word, aarch32::ItState{static_cast<std::uint8_t>(itstate)},
                profile)
          : aarch32::decode_a32(word, profile);
  if (counting_exits.load(std::memory_order_relaxed)) {
    form_exits[static_cast<std::size_t>(inst.form)].fetch_add(
        1, std::memory_order_relaxed);
    exit_total.fetch_add(1, std::memory_order_relaxed);
  }
  const auto transfer = a32::describe_transfer(inst);
  if (transfer.kind != Transfer::Kind::exclusive)
    return execute_in_library(*cpu, *tlb, inst, key, itstate);

  /* The exclusives: serialized with the other vCPUs' stores while a software
   * reservation exists, and creating one registers it (the A64 exclusive
   * helper's rules). */
  const bool establishes = transfer.load;
  const bool uses_software_reservation =
      establishes || cpu->exclusive_size != 0U;
  const auto run = [&]() noexcept {
    const bool registered = establishes && begin_jit_reservation(*cpu);
    const auto result = execute_exclusive(*cpu, *tlb, inst, key, itstate, transfer);
    if (establishes && cpu->exclusive_size != 0U &&
        current_jit_store_participant != nullptr)
      current_jit_store_participant->reservation_cpu.store(
          cpu, std::memory_order_seq_cst);
    if (registered && cpu->exclusive_size == 0) {
      if (current_jit_store_participant != nullptr &&
          current_jit_store_participant->reservation_cpu.load(
              std::memory_order_seq_cst) == cpu)
        current_jit_store_participant->reservation_cpu.store(
            nullptr, std::memory_order_seq_cst);
      end_jit_reservation(*cpu);
    }
    return result;
  };
  if (uses_software_reservation) {
    JitExclusiveOperationScope operation;
    return run();
  }
  return run();
}

} // namespace uml::cpu::arm64
