// SPDX-License-Identifier: GPL-2.0-or-later
//
// The terminal slow path of the AArch32 translation (a32_helpers.hpp): one
// memory instruction executed in C++ on the register file the generated code
// spilled. It runs what the inline lowering declines (an access that crosses
// a page, a misaligned one that must fault, the exclusives and the rare
// forms) with exact fault ordering: a store writes nothing before every page
// it touches is translated, and a load changes no register before its last
// element is read.
//
// Interim: the AArch32 semantic library (design 3.2, lane A32-S) replaces this
// file's execution of the transfer classes when it exists.

#include "a32_helpers.hpp"

#include "a32_operations.hpp"
#include "a32_transfer.hpp"

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/jit/jit_publication.hpp"
#include "cpu/arm64/jit/jit_runtime_contract.hpp"
#include "cpu/arm64/jit/jit_semantic_reservation.hpp"
#include "cpu/arm64/memory.hpp"

#include <algorithm>
#include <array>
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

enum class Outcome : std::uint8_t {
  retired,
  /* A translation failed: the Tlb holds the address and access. */
  fault,
  /* The access was misaligned for its class. */
  alignment,
  illegal,
};

/* The result of an address offset's shift (the carry-out is unused). */
[[nodiscard]] std::uint32_t apply_shift(std::uint32_t value, SRType type,
                                        unsigned amount,
                                        bool carry_in) noexcept {
  switch (type) {
  case SRType::LSL:
    return amount >= 32U ? 0U : value << amount;
  case SRType::LSR:
    return amount >= 32U ? 0U : value >> amount;
  case SRType::ASR:
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value) >>
                                      std::min(amount, 31U));
  case SRType::ROR:
    return std::rotr(value, static_cast<int>(amount & 31U));
  case SRType::RRX:
    return (carry_in ? 0x8000'0000U : 0U) | value >> 1;
  }
  return value;
}

class Execution final {
public:
  Execution(CpuState &cpu, Tlb &tlb, const Instruction &inst,
            std::uint64_t key, const Transfer &transfer)
      : cpu_{cpu}, tlb_{tlb}, memory_{tlb}, inst_{inst}, transfer_{transfer},
        thumb_{aarch32::key_is_thumb(key)}, pc_{aarch32::key_pc(key)} {}

  [[nodiscard]] Outcome run() {
    switch (transfer_.kind) {
    case Transfer::Kind::single: return single();
    case Transfer::Kind::pair: return pair();
    case Transfer::Kind::multiple: return multiple();
    case Transfer::Kind::table: return table();
    case Transfer::Kind::exclusive: return exclusive();
    case Transfer::Kind::prefetch: return retire();
    case Transfer::Kind::none: break;
    }
    return Outcome::illegal;
  }

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
  [[nodiscard]] bool carry() const noexcept {
    return (cpu_.nzcv >> 29 & 1U) != 0U;
  }

  /* The instruction retired and execution continues after it. */
  [[nodiscard]] Outcome retire() noexcept {
    cpu_.pc = thumb_ ? aarch32::t32_key(pc_ + inst_.length)
                     : aarch32::a32_key(pc_ + inst_.length);
    return Outcome::retired;
  }
  /* A load wrote the PC: LoadWritePC interworks on bit 0. */
  [[nodiscard]] Outcome branch_to(std::uint32_t value) noexcept {
    cpu_.pc = aarch32::key_from_target(value);
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

  [[nodiscard]] bool load_value(std::uint32_t address, unsigned bytes,
                                std::uint64_t &value) {
    std::array<std::byte, 8> raw{};
    if (!memory_.read(address, std::span{raw}.first(bytes)))
      return false;
    value = 0;
    std::memcpy(&value, raw.data(), bytes);
    return true;
  }
  [[nodiscard]] bool store_value(std::uint32_t address, unsigned bytes,
                                 std::uint64_t value) {
    std::array<std::byte, 8> raw{};
    std::memcpy(raw.data(), &value, bytes);
    return sem::ordinary_store(memory_, address,
                               std::span<const std::byte>{raw}.first(bytes));
  }

  /* The offset a register or immediate addressing form adds. */
  [[nodiscard]] std::uint32_t offset() const noexcept {
    if (!transfer_.register_offset)
      return transfer_.imm;
    return apply_shift(read(transfer_.m), transfer_.shift_type,
                       transfer_.shift_amount, carry());
  }

  struct Addressing final {
    std::uint32_t address;
    std::uint32_t written_back;
  };
  [[nodiscard]] Addressing addressing() const noexcept {
    if (transfer_.literal) {
      const auto base = (pc_ + (thumb_ ? 4U : 8U)) & ~3U;
      return {transfer_.add ? base + transfer_.imm : base - transfer_.imm, 0};
    }
    const auto base = read(transfer_.n);
    const auto displaced = transfer_.add ? base + offset() : base - offset();
    return {transfer_.index ? displaced : base, displaced};
  }

  [[nodiscard]] Outcome single() {
    const auto where = addressing();
    const unsigned bytes = transfer_.bytes;
    if (transfer_.load) {
      std::uint64_t value;
      if (!load_value(where.address, bytes, value))
        return memory_fault(false);
      auto word = static_cast<std::uint32_t>(value);
      if (transfer_.sign_extend)
        word = bytes == 1U ? static_cast<std::uint32_t>(
                                 static_cast<std::int32_t>(
                                     static_cast<std::int8_t>(word)))
                           : static_cast<std::uint32_t>(
                                 static_cast<std::int32_t>(
                                     static_cast<std::int16_t>(word)));
      if (transfer_.wback)
        write(transfer_.n, where.written_back);
      if (transfer_.t == 15U)
        return branch_to(word);
      write(transfer_.t, word);
      return retire();
    }
    if (!store_value(where.address, bytes, read(transfer_.t)))
      return memory_fault(true);
    if (transfer_.wback)
      write(transfer_.n, where.written_back);
    return retire();
  }

  [[nodiscard]] Outcome pair() {
    const auto where = addressing();
    /* MemA: a doubleword transfer needs word alignment. */
    if ((where.address & 3U) != 0U)
      return alignment_fault(where.address, !transfer_.load);
    if (transfer_.load) {
      std::uint64_t value;
      if (!load_value(where.address, 8, value))
        return memory_fault(false);
      if (transfer_.wback)
        write(transfer_.n, where.written_back);
      write(transfer_.t, static_cast<std::uint32_t>(value));
      write(transfer_.t2, static_cast<std::uint32_t>(value >> 32));
      return retire();
    }
    const std::uint64_t value = std::uint64_t{read(transfer_.t)} |
                                std::uint64_t{read(transfer_.t2)} << 32;
    if (!store_value(where.address, 8, value))
      return memory_fault(true);
    if (transfer_.wback)
      write(transfer_.n, where.written_back);
    return retire();
  }

  [[nodiscard]] Outcome multiple() {
    const unsigned count =
        std::popcount(static_cast<unsigned>(transfer_.registers));
    const std::uint32_t base = read(transfer_.n);
    const std::uint32_t span = 4U * count;
    std::uint32_t start;
    std::uint32_t final_base;
    switch (transfer_.mode) {
    case Transfer::Mode::ia: start = base; final_base = base + span; break;
    case Transfer::Mode::ib: start = base + 4U; final_base = base + span; break;
    case Transfer::Mode::da:
      start = base - span + 4U;
      final_base = base - span;
      break;
    default: start = base - span; final_base = base - span; break;
    }
    if ((start & 3U) != 0U)
      return alignment_fault(start, !transfer_.load);
    std::array<std::uint32_t, 16> words{};
    std::array<std::byte, 64> raw{};
    if (transfer_.load) {
      if (!memory_.read(start, std::span{raw}.first(span)))
        return memory_fault(false);
      std::memcpy(words.data(), raw.data(), span);
      unsigned index = 0;
      std::uint32_t pc_value = 0;
      bool loads_pc = false;
      for (unsigned r = 0; r < 16U; ++r) {
        if ((transfer_.registers >> r & 1U) == 0U)
          continue;
        if (r == 15U) {
          pc_value = words[index];
          loads_pc = true;
        } else {
          write(r, words[index]);
        }
        ++index;
      }
      if (transfer_.wback)
        write(transfer_.n, final_base);
      return loads_pc ? branch_to(pc_value) : retire();
    }
    unsigned index = 0;
    for (unsigned r = 0; r < 16U; ++r)
      if ((transfer_.registers >> r & 1U) != 0U)
        words[index++] = read(r);
    std::memcpy(raw.data(), words.data(), span);
    if (!sem::ordinary_store(memory_, start,
                             std::span<const std::byte>{raw}.first(span)))
      return memory_fault(true);
    if (transfer_.wback)
      write(transfer_.n, final_base);
    return retire();
  }

  [[nodiscard]] Outcome table() {
    const std::uint32_t index = read(transfer_.m);
    const std::uint32_t address =
        read(transfer_.n) + (transfer_.halfword_table ? index * 2U : index);
    std::uint64_t entry;
    if (!load_value(address, transfer_.bytes, entry))
      return memory_fault(false);
    const std::uint32_t target =
        pc_ + 4U + 2U * static_cast<std::uint32_t>(entry);
    cpu_.pc = aarch32::t32_key(target);
    return Outcome::retired;
  }

  [[nodiscard]] Outcome exclusive() {
    const std::uint32_t address = read(transfer_.n);
    const unsigned bytes = transfer_.bytes;
    if ((address & (bytes - 1U)) != 0U)
      return alignment_fault(address, !transfer_.load);
    if (transfer_.load) {
      std::scoped_lock lock{sem::exclusive_memory_mutex};
      std::uint64_t value = 0;
      std::uintptr_t key = 0;
      if (!sem::atomic_load_value(memory_, address, bytes,
                                  std::memory_order_relaxed, value, key))
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
          memory_, address, bytes, value, std::memory_order_relaxed, &key,
          stored_key);
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

[[nodiscard]] std::uint64_t execute(CpuState &cpu, Tlb &tlb,
                                    const Instruction &inst, std::uint64_t key,
                                    std::uint32_t itstate,
                                    const Transfer &transfer) noexcept {
  Execution execution{cpu, tlb, inst, key, transfer};
  const auto outcome = execution.run();
  switch (outcome) {
  case Outcome::retired:
    cpu.a32_itstate = inst.next_it.bits();
    return pack_jit_result(Event::continued, 1);
  case Outcome::fault:
  case Outcome::alignment:
    /* The instruction did not retire: the task resumes at it, in the IT
     * state it was entered in. */
    cpu.a32_itstate = static_cast<std::uint8_t>(itstate);
    tlb.fault_address = execution.fault_address();
    tlb.fault_access =
        execution.fault_is_write() ? Access::write : Access::read;
    tlb.fault_alignment = outcome == Outcome::alignment ? 1U : 0U;
    return pack_jit_result(Event::memory_fault, 0);
  case Outcome::illegal:
    break;
  }
  cpu.a32_itstate = static_cast<std::uint8_t>(itstate);
  return pack_jit_result(Event::illegal_instruction, 0);
}

} // namespace

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
  const auto transfer = a32::describe_transfer(inst);
  /* Generated code routes only transfers here; anything else is an internal
   * error and is never interpreted. */
  if (transfer.kind == Transfer::Kind::none)
    return pack_jit_result(Event::fatal, 0);
  if (transfer.kind != Transfer::Kind::exclusive)
    return execute(*cpu, *tlb, inst, key, itstate, transfer);

  /* The exclusives: serialized with the other vCPUs' stores while a software
   * reservation exists, and creating one registers it (the A64 exclusive
   * helper's rules). */
  const bool establishes = transfer.load;
  const bool uses_software_reservation =
      establishes || cpu->exclusive_size != 0U;
  const auto run = [&]() noexcept {
    const bool registered = establishes && begin_jit_reservation(*cpu);
    const auto result = execute(*cpu, *tlb, inst, key, itstate, transfer);
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
