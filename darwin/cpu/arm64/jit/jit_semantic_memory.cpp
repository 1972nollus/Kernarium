// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_semantic_memory.hpp"
#include "jit_publication.hpp"

#include "cpu/arm64/dispatcher.hpp"
#include "cpu/arm64/detail/floating_exception.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/memory_transfer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <span>

namespace uml::cpu::arm64 {

using ::uml::memory::Access;

void
activate_native_fp_environment(CpuState &cpu) noexcept {
  if (cpu.gadget_fp_active)
    return;
  __asm__ volatile("mrs %0, fpcr" : "=r"(cpu.gadget_host_fpcr) : : "memory");
  __asm__ volatile("mrs %0, fpsr" : "=r"(cpu.gadget_host_fpsr) : : "memory");
  cpu.gadget_fp_active = true;
  const std::uint64_t fpcr = arm64::detail::host_fpcr(cpu.fpcr);
  const std::uint64_t fpsr = cpu.fpsr;
  __asm__ volatile("msr fpcr, %0" : : "r"(fpcr) : "memory");
  __asm__ volatile("msr fpsr, %0" : : "r"(fpsr) : "memory");
}

void
activate_native_fpmr_environment(CpuState &cpu) noexcept {
  if (cpu.gadget_fpmr_active)
    return;
  __asm__ volatile("mrs %0, S3_3_C4_C4_2"
                   : "=r"(cpu.gadget_host_fpmr)
                   :
                   : "memory");
  cpu.gadget_fpmr_active = true;
  const std::uint64_t guest_fpmr = cpu.fpmr;
  __asm__ volatile("msr S3_3_C4_C4_2, %0"
                   :
                   : "r"(guest_fpmr)
                   : "memory");
}

namespace {

void materialize_semantic_cpu_state(CpuState &cpu) noexcept {
  /*
   * A terminal generated-code helper runs outside the native register
   * environment. Commit any retained guest status and restore the host
   * environment before the reference semantics can inspect CpuState.
   */
  arm64::restore_native_fp_environment(cpu);
  cpu.access_fp_state();
}

extern "C" std::uint64_t
uml_arm64_jit_alignment_fault(CpuState *cpu, Tlb *tlb,
                              std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);
  const unsigned base = (instruction >> 5) & 31U;
  tlb->fault_alignment = 1;
  tlb->fault_address = cpu->read_register(base, true);
  const unsigned operation = (instruction >> 22) & 3U;
  const bool vector = (instruction & 0x0400'0000U) != 0;
  const bool authenticated_load =
      (instruction & 0xff20'0400U) == 0xf820'0400U;
  const bool load = authenticated_load ||
                    (vector ? (operation & 1U) != 0U : operation != 0U);
  tlb->fault_access = load ? Access::read : Access::write;
  return pack_jit_result(Event::memory_fault, 0);
}

extern "C" void *uml_arm64_jit_tlb_translate(Tlb *tlb, std::uint64_t address,
                                             unsigned access) noexcept {
  if (tlb == nullptr || access > 1)
    return nullptr;
  return tlb->translate(address, access == 0 ? Access::read : Access::write);
}

extern "C" void *
uml_arm64_jit_tlb_translate_preserving(Tlb *tlb, std::uint64_t address,
                                       unsigned access) noexcept;
extern "C" void *
uml_arm64_jit_tlb_translate_preserving_fpmr(Tlb *tlb,
                                            std::uint64_t address,
                                            unsigned access) noexcept;

[[nodiscard]] bool jit_pair_read(Tlb &tlb, std::uint64_t address,
                                 std::span<std::byte> bytes) noexcept {
  while (!bytes.empty()) {
    auto *source = tlb.translate(address, Access::read);
    if (source == nullptr)
      return false;
    const auto count = std::min(
        bytes.size(),
        static_cast<std::size_t>(page_size - (address & (page_size - 1U))));
    std::memcpy(bytes.data(), source, count);
    address += count;
    bytes = bytes.subspan(count);
  }
  return true;
}

[[nodiscard]] bool jit_pair_write(Tlb &tlb, std::uint64_t address,
                                  std::span<const std::byte> bytes) noexcept {
  /* Pair stores are architecturally one memory access. Translate every
   * page before the first host store so a second-page fault cannot expose a
   * partial guest update. A pair spans at most two supported guest pages.
   */
  std::array<std::byte *, 2> destinations{};
  std::array<std::size_t, 2> lengths{};
  std::size_t pages = 0;
  std::size_t completed = 0;
  while (completed != bytes.size()) {
    if (pages == destinations.size())
      return false;
    const auto current = address + completed;
    auto *destination = tlb.translate(current, Access::write);
    if (destination == nullptr)
      return false;
    const auto count = std::min(
        bytes.size() - completed,
        static_cast<std::size_t>(page_size - (current & (page_size - 1U))));
    destinations[pages] = destination;
    lengths[pages] = count;
    ++pages;
    completed += count;
  }
  completed = 0;
  for (std::size_t page = 0; page < pages; ++page) {
    std::memcpy(destinations[page], bytes.data() + completed, lengths[page]);
    completed += lengths[page];
  }
  return true;
}

class JitSemanticMemory final : public ::uml::memory::GuestAddressSpace {
public:
  explicit JitSemanticMemory(Tlb &tlb,
                             bool scalar_ordered_atomics = false) noexcept
      : tlb_{tlb}, scalar_ordered_atomics_{scalar_ordered_atomics} {}

  bool read(std::uint64_t address, std::span<std::byte> destination) override {
    if (jit_pair_read(tlb_, address, destination))
      return true;
    record_fault();
    return false;
  }

  bool write(std::uint64_t address,
             std::span<const std::byte> source) override {
    std::size_t completed = 0;
    while (completed < source.size()) {
      const auto current = address + completed;
      tlb_.discard(current);
      completed += std::min(
          source.size() - completed,
          static_cast<std::size_t>(page_size - (current & (page_size - 1U))));
    }
    if (jit_pair_write(tlb_, address, source))
      return true;
    record_fault();
    return false;
  }

  bool validate_access(std::uint64_t address, std::size_t size,
                       ::uml::memory::MemoryAccess access) override {
    const auto direction = access == ::uml::memory::MemoryAccess::write
                               ? Access::write
                               : Access::read;
    std::size_t completed = 0;
    while (completed < size) {
      const auto current = address + completed;
      if (tlb_.translate(current, direction) == nullptr) {
        record_fault();
        return false;
      }
      completed += std::min(
          size - completed,
          static_cast<std::size_t>(
              page_size - (current & (page_size - 1U))));
    }
    return true;
  }

  /*
   * Returning no direct pointer deliberately selects the interpreter's
   * ordinary-load/store atomic fallback. Host exclusive instructions cannot
   * be restarted safely after a Darwin shadow-map protection fault.
   *
   * A scalar ordered store is the single exception, and only
   * uml_arm64_jit_execute_ordered enables it. That form lowers to one plain
   * release store rather than an exclusive sequence, so it is restartable,
   * and it must be single-copy atomic: guest memory shared with a native host
   * thread - the Venus command ring reached through the vhost-user
   * shared-memory aperture - is read there with real atomics, and the
   * byte-wise fallback lets that reader observe a half-updated word that was
   * never architecturally visible.
   */
  std::byte *atomic_pointer(std::uint64_t address, std::size_t size,
                            ::uml::memory::MemoryAccess access) override {
    if (!scalar_ordered_atomics_ || size == 0 ||
        size > sizeof(std::uint64_t) || (size & (size - 1U)) != 0 ||
        (address & (size - 1U)) != 0)
      return nullptr;
    const auto direction = ::uml::memory::translated_access(access);
    /*
     * The dynamic loader can change COW permissions within the same page
     * between ordered stores. Revalidate the write translation at this cold
     * helper boundary instead of retaining a pre-transition host pointer,
     * exactly as the byte-wise path does before it copies.
     */
    if (direction == Access::write)
      tlb_.discard(address);
    return tlb_.translate(address, direction);
  }

  std::byte *reservation_pointer(
      std::uint64_t address, std::size_t size,
      ::uml::memory::MemoryAccess access) override {
    if (size == 0 ||
        size > page_size - (address & (page_size - 1U)))
      return nullptr;
    return tlb_.translate(
        address, ::uml::memory::translated_access(access));
  }

  bool read_allocation_tag(std::uint64_t address,
                           std::uint8_t &tag) override {
    if (tlb_.read_allocation_tag(address, tag))
      return true;
    record_fault();
    return false;
  }

  bool write_allocation_tag(std::uint64_t address,
                            std::uint8_t tag) override {
    if (tlb_.write_allocation_tag(address, tag))
      return true;
    record_fault();
    return false;
  }

  ::uml::memory::MemoryFault last_fault() const noexcept override {
    return fault_;
  }

  [[nodiscard]] bool has_fault() const noexcept override {
    return fault_recorded_;
  }

private:
  void record_fault() noexcept {
    fault_ = {tlb_.fault_address,
              ::uml::memory::guest_access(tlb_.fault_access)};
    fault_recorded_ = true;
  }

  Tlb &tlb_;
  bool scalar_ordered_atomics_{};
  ::uml::memory::MemoryFault fault_{};
  bool fault_recorded_{};
};

void record_semantic_fault(Tlb &tlb, const JitSemanticMemory &memory,
                           const arm64::RunResult &result) noexcept {
  tlb.fault_address =
      !result.alignment && memory.has_fault() ? memory.last_fault().address
                                             : result.address;
  tlb.fault_access = result.write ? Access::write : Access::read;
  tlb.fault_alignment = result.alignment;
}

extern "C" std::uint64_t
uml_arm64_jit_execute_exclusive(CpuState *cpu, Tlb *tlb,
                                std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);
  const bool establishes_reservation =
      (instruction & 0x00c0'0000U) == 0x0040'0000U;
  const bool uses_software_reservation =
      establishes_reservation || cpu->exclusive_size != 0U;
  const auto execute = [&]() noexcept {
    const bool registered =
        establishes_reservation && begin_jit_reservation(*cpu);
    JitSemanticMemory memory{*tlb};
    const auto result =
        arm64::Dispatcher{}.execute(*cpu, memory, instruction);
    if (establishes_reservation && cpu->exclusive_size != 0U &&
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
    switch (result.reason) {
    case arm64::StopReason::continued:
    case arm64::StopReason::budget_exhausted:
      return pack_jit_result(Event::continued, result.instructions);
    case arm64::StopReason::memory_fault:
      record_semantic_fault(*tlb, memory, result);
      return pack_jit_result(Event::memory_fault, result.instructions);
    case arm64::StopReason::gcs_fault:
      return pack_jit_result(Event::gcs_fault, result.instructions);
    case arm64::StopReason::illegal_instruction:
    case arm64::StopReason::unavailable_feature:
      return pack_jit_result(Event::illegal_instruction,
                             result.instructions);
    case arm64::StopReason::breakpoint:
      return pack_jit_result(Event::breakpoint, result.instructions);
    case arm64::StopReason::system_call:
      return pack_jit_result(Event::system_call, result.instructions);
    case arm64::StopReason::floating_point_trap:
      return pack_jit_result(Event::floating_point_trap,
                             result.instructions);
    case arm64::StopReason::unsupported_instruction:
      return pack_jit_result(Event::illegal_instruction,
                             result.instructions);
    }
    return pack_jit_result(Event::fatal, 0);
  };
  /*
   * Native compare/exchange and atomic instructions already provide their
   * own inter-vCPU serialization. Only the semantic fallback which creates
   * or consumes CpuState's software reservation needs the publication gate.
   */
  if (uses_software_reservation) {
    JitExclusiveOperationScope operation;
    return execute();
  }
  return execute();
}

[[nodiscard]] std::uint64_t execute_semantic_instruction(
    CpuState &cpu, Tlb &tlb, std::uint32_t instruction,
    std::uint32_t *syndrome, bool scalar_ordered_atomics) noexcept {
  materialize_semantic_cpu_state(cpu);
  JitSemanticMemory memory{tlb, scalar_ordered_atomics};
  const auto result = arm64::Dispatcher{}.execute(cpu, memory, instruction);
  if (syndrome != nullptr)
    *syndrome = result.syndrome;
  switch (result.reason) {
  case arm64::StopReason::continued:
  case arm64::StopReason::budget_exhausted:
    return pack_jit_result(Event::continued, result.instructions);
  case arm64::StopReason::memory_fault:
    record_semantic_fault(tlb, memory, result);
    return pack_jit_result(Event::memory_fault, result.instructions);
  case arm64::StopReason::gcs_fault:
    return pack_jit_result(Event::gcs_fault, result.instructions);
  case arm64::StopReason::illegal_instruction:
  case arm64::StopReason::unavailable_feature:
    return pack_jit_result(Event::illegal_instruction, result.instructions);
  case arm64::StopReason::breakpoint:
    return pack_jit_result(Event::breakpoint, result.instructions);
  case arm64::StopReason::system_call:
    return pack_jit_result(Event::system_call, result.instructions);
  case arm64::StopReason::floating_point_trap:
    return pack_jit_result(Event::floating_point_trap, result.instructions);
  case arm64::StopReason::unsupported_instruction:
    return pack_jit_result(Event::illegal_instruction, result.instructions);
  }
  return pack_jit_result(Event::fatal, 0);
}

extern "C" std::uint64_t
uml_arm64_jit_execute_memory_with_syndrome(
    CpuState *cpu, Tlb *tlb, std::uint32_t instruction,
    std::uint32_t *syndrome) noexcept {
  if (syndrome != nullptr)
    *syndrome = 0;
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  return execute_semantic_instruction(*cpu, *tlb, instruction, syndrome,
                                      false);
}

extern "C" std::uint64_t
uml_arm64_jit_execute_memory(CpuState *cpu, Tlb *tlb,
                             std::uint32_t instruction) noexcept {
  return uml_arm64_jit_execute_memory_with_syndrome(
      cpu, tlb, instruction, nullptr);
}

extern "C" std::uint64_t
uml_arm64_jit_execute_gcs(CpuState *cpu, Tlb *tlb,
                          std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);
  JitSemanticMemory memory{*tlb};
  const auto result = arm64::Dispatcher{}.execute(*cpu, memory, instruction);
  switch (result.reason) {
  case arm64::StopReason::continued:
  case arm64::StopReason::budget_exhausted:
    return pack_jit_result(Event::continued, result.instructions);
  case arm64::StopReason::memory_fault:
    record_semantic_fault(*tlb, memory, result);
    return pack_jit_result(Event::memory_fault, result.instructions);
  case arm64::StopReason::gcs_fault:
    return pack_jit_result(Event::gcs_fault, result.instructions);
  case arm64::StopReason::illegal_instruction:
  case arm64::StopReason::unavailable_feature:
    return pack_jit_result(Event::illegal_instruction, result.instructions);
  case arm64::StopReason::breakpoint:
    return pack_jit_result(Event::breakpoint, result.instructions);
  case arm64::StopReason::system_call:
    return pack_jit_result(Event::system_call, result.instructions);
  case arm64::StopReason::floating_point_trap:
    return pack_jit_result(Event::floating_point_trap, result.instructions);
  case arm64::StopReason::unsupported_instruction:
    return pack_jit_result(Event::illegal_instruction, result.instructions);
  }
  return pack_jit_result(Event::fatal, 0);
}

extern "C" std::uint64_t
uml_arm64_jit_execute_pair(CpuState *cpu, Tlb *tlb,
                           std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);

  const bool vector = (instruction & 0x0400'0000U) != 0;
  const bool load = (instruction & 0x0040'0000U) != 0;
  const unsigned operation = instruction >> 30;
  const unsigned mode = (instruction >> 23) & 3U;
  const std::size_t size =
      vector            ? (operation >= 2U ? 16U : std::size_t{4} << operation)
      : operation >= 2U ? 8U
                        : 4U;
  const auto displacement = sign_extend((instruction >> 15) & 0x7fU, 7) *
                            static_cast<std::int64_t>(size);
  const unsigned base_register = (instruction >> 5) & 31U;
  const auto tagged_base = cpu->read_register(base_register, true);
  if (base_register == 31U && (tagged_base & 15U) != 0) {
    tlb->fault_alignment = 1;
    tlb->fault_address = tagged_base;
    tlb->fault_access = load ? Access::read : Access::write;
    return pack_jit_result(Event::memory_fault, 0);
  }
  const auto adjusted =
      tagged_base + static_cast<std::uint64_t>(displacement);
  const auto tagged_address = mode == 1U ? tagged_base : adjusted;
  const bool tagged =
      base_register != 31U && cpu->tagged_addresses_enabled();
  const auto address =
      tagged ? tagged_address & 0x00ff'ffff'ffff'ffffULL : tagged_address;
  const unsigned first = instruction & 31U;
  const unsigned second = (instruction >> 10) & 31U;
  std::array<std::byte, 32> bytes{};
  const auto transfer = std::span{bytes}.first(size * 2U);
  const auto direction = load ? Access::read : Access::write;
  const bool tagchecked = mode == 1U || mode == 3U || base_register != 31U;
  if (tagchecked && cpu->mte_enabled && !cpu->pstate_tco) {
    /* A pair is one architectural memory access. Resolve every page and
     * allocation granule before either destination or memory can change. */
    std::size_t completed = 0;
    while (completed < transfer.size()) {
      const auto current = address + completed;
      if (tlb->translate(current, direction) == nullptr)
        return pack_jit_result(Event::memory_fault, 0);
      completed +=
          std::min(transfer.size() - completed,
                   static_cast<std::size_t>(
                       page_size - (current & (page_size - 1U))));
    }
    const auto logical_tag = base_register != 31U && cpu->mte_enabled
                                 ? static_cast<std::uint8_t>(tagged_address >> 56) & 15U
                                 : 0U;
    for (std::size_t checked = 0; checked < transfer.size();) {
      const auto current = address + checked;
      std::uint8_t allocation_tag{};
      if (!tlb->read_allocation_tag(current, allocation_tag) ||
          allocation_tag != logical_tag) {
        tlb->fault_address = current;
        tlb->fault_access = direction;
        return pack_jit_result(Event::memory_fault, 0);
      }
      checked += std::min(
          transfer.size() - checked,
          static_cast<std::size_t>(16U - (current & 15U)));
    }
  }

  if (load) {
    if (!jit_pair_read(*tlb, address, transfer))
      return pack_jit_result(Event::memory_fault, 0);
    if (vector) {
      cpu->vectors[first].bytes.fill(std::byte{});
      cpu->vectors[second].bytes.fill(std::byte{});
      std::memcpy(cpu->vectors[first].bytes.data(), bytes.data(), size);
      std::memcpy(cpu->vectors[second].bytes.data(), bytes.data() + size, size);
    } else {
      std::uint64_t first_value{};
      std::uint64_t second_value{};
      std::memcpy(&first_value, bytes.data(), size);
      std::memcpy(&second_value, bytes.data() + size, size);
      if (operation == 1U) {
        first_value = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(static_cast<std::int32_t>(first_value)));
        second_value = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(static_cast<std::int32_t>(second_value)));
      }
      cpu->write_register(first, first_value, false);
      cpu->write_register(second, second_value, false);
    }
  } else {
    if (vector) {
      std::memcpy(bytes.data(), cpu->vectors[first].bytes.data(), size);
      std::memcpy(bytes.data() + size, cpu->vectors[second].bytes.data(), size);
    } else {
      const auto first_value = cpu->read_register(first, false);
      const auto second_value = cpu->read_register(second, false);
      std::memcpy(bytes.data(), &first_value, size);
      std::memcpy(bytes.data() + size, &second_value, size);
    }
    if (!jit_pair_write(*tlb, address, transfer))
      return pack_jit_result(Event::memory_fault, 0);
  }

  bool writeback = mode == 1U || mode == 3U;
  if (writeback && load && !vector && base_register != 31U &&
      (base_register == first || base_register == second))
    writeback = false;
  if (writeback)
    cpu->write_register(base_register, adjusted, true);
  cpu->pc += sizeof(instruction);
  return pack_jit_result(Event::continued, 1);
}

/*
 * Scalar ordered accesses are single-copy atomic, with acquire or release
 * ordering where selected by the instruction. Guest memory shared with a
 * native host thread - the Venus command ring is reached over the vhost-user
 * shared-memory aperture - is observed by that thread with real atomics, so a
 * byte-wise copy here races: the renderer can load a tail word mid-store and
 * see a value that was never architecturally visible. Perform the access as
 * one host atomic whenever the translation is suitably aligned. The stronger
 * sequentially-consistent host order also covers every scalar ordered form
 * routed through this helper.
 */
template <typename Integer>
[[nodiscard]] std::uint64_t ordered_load_at(void *pointer) noexcept {
  return std::atomic_ref<Integer>{*static_cast<Integer *>(pointer)}.load(
      std::memory_order_seq_cst);
}

[[nodiscard]] bool ordered_access_is_atomic(const void *pointer,
                                            std::size_t size) noexcept {
  return size <= sizeof(std::uint64_t) &&
         (reinterpret_cast<std::uintptr_t>(pointer) & (size - 1U)) == 0;
}

extern "C" std::uint64_t
uml_arm64_jit_execute_ordered(CpuState *cpu, Tlb *tlb,
                              std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  const bool load = (instruction & 0x0040'0000U) != 0;
  /*
   * Ordered stores must invalidate software reservations even when an alias
   * writes the same bits, and must revalidate write access. The reference
   * semantics own both rules, so route the store there; asking for direct
   * scalar atomics makes that path store the value with one host release
   * store instead of copying it a byte at a time.
   */
  if (!load)
    return execute_semantic_instruction(*cpu, *tlb, instruction, nullptr,
                                        true);
  materialize_semantic_cpu_state(*cpu);
  const unsigned size_shift = instruction >> 30;
  const std::size_t size = std::size_t{1} << size_shift;
  const unsigned base = (instruction >> 5) & 31U;
  const unsigned transfer = instruction & 31U;
  const auto tagged_address = cpu->read_register(base, true);
  const auto address = cpu->untag_data_address(tagged_address, base == 31U);
  if ((address & (size - 1U)) != 0) {
    tlb->fault_alignment = 1;
    tlb->fault_address = address;
    tlb->fault_access = Access::read;
    return pack_jit_result(Event::memory_fault, 0);
  }
  auto *source = tlb->translate(address, Access::read);
  if (source == nullptr)
    return pack_jit_result(Event::memory_fault, 0);
  std::uint64_t value = 0;
  if (ordered_access_is_atomic(source, size)) {
    switch (size) {
    case 1: value = ordered_load_at<std::uint8_t>(source); break;
    case 2: value = ordered_load_at<std::uint16_t>(source); break;
    case 4: value = ordered_load_at<std::uint32_t>(source); break;
    default: value = ordered_load_at<std::uint64_t>(source); break;
    }
  } else {
    std::memcpy(&value, source, size);
    std::atomic_thread_fence(std::memory_order_acquire);
  }
  cpu->write_register(transfer, value, false);
  cpu->pc += sizeof(instruction);
  return pack_jit_result(Event::continued, 1);
}

extern "C" std::uint64_t
uml_arm64_jit_execute_simd_structure(CpuState *cpu, Tlb *tlb,
                                     std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);
  const bool single = (instruction & 0xbf80'0000U) == 0x0d00'0000U ||
                      (instruction & 0xbf80'0000U) == 0x0d80'0000U;
  const bool load = (instruction & 0x0040'0000U) != 0;
  const bool post_index = (instruction & 0x0080'0000U) != 0;
  const unsigned offset_register = (instruction >> 16) & 31U;
  const unsigned base_register = (instruction >> 5) & 31U;
  const unsigned first_register = instruction & 31U;
  const auto tagged_address = cpu->read_register(base_register, true);
  if (base_register == 31U && (tagged_address & 15U) != 0) {
    tlb->fault_alignment = 1;
    tlb->fault_address = tagged_address;
    tlb->fault_access = load ? Access::read : Access::write;
    return pack_jit_result(Event::memory_fault, 0);
  }

  unsigned register_count = 0;
  std::size_t element_size = 0;
  std::size_t vector_size = (instruction & 0x4000'0000U) != 0 ? 16U : 8U;
  std::size_t lane_offset = 0;
  bool interleaved = false;
  bool replicate = false;
  if (!single) {
    const unsigned opcode = (instruction >> 12) & 15U;
    interleaved = opcode == 8U || opcode == 4U || opcode == 0U;
    register_count = opcode == 7U    ? 1U
                     : opcode == 10U ? 2U
                     : opcode == 6U  ? 3U
                     : opcode == 2U  ? 4U
                     : opcode == 8U  ? 2U
                     : opcode == 4U  ? 3U
                     : opcode == 0U  ? 4U
                                     : 0U;
    const unsigned size = (instruction >> 10) & 3U;
    if (register_count == 0U || (!post_index && offset_register != 0U) ||
        (interleaved && vector_size == 8U && size == 3U))
      return pack_jit_result(Event::illegal_instruction, 0);
    element_size = std::size_t{1} << size;
  } else {
    const unsigned extra_register = (instruction >> 21) & 1U;
    const unsigned opcode = (instruction >> 13) & 7U;
    const unsigned scale_bit = (instruction >> 12) & 1U;
    const unsigned size = (instruction >> 10) & 3U;
    const unsigned scale = opcode >> 1;
    if (!post_index && offset_register != 0U)
      return pack_jit_result(Event::illegal_instruction, 0);
    if (scale == 3U) {
      if (!load || scale_bit != 0U)
        return pack_jit_result(Event::illegal_instruction, 0);
      replicate = true;
      register_count = opcode == 6U ? 1U + extra_register : 3U + extra_register;
      element_size = std::size_t{1} << size;
    } else {
      register_count =
          (opcode & 1U) != 0U ? 3U + extra_register : 1U + extra_register;
      std::size_t lane = 0;
      if (scale == 0U) {
        element_size = 1U;
        lane = (static_cast<std::size_t>((instruction >> 30) & 1U) << 3) |
               (scale_bit << 2) | size;
      } else if (scale == 1U) {
        if ((size & 1U) != 0U)
          return pack_jit_result(Event::illegal_instruction, 0);
        element_size = 2U;
        lane = (static_cast<std::size_t>((instruction >> 30) & 1U) << 2) |
               (scale_bit << 1) | (size >> 1);
      } else {
        if ((size & 2U) != 0U || ((size & 1U) != 0U && scale_bit != 0U))
          return pack_jit_result(Event::illegal_instruction, 0);
        element_size = (size & 1U) != 0U ? 8U : 4U;
        lane = (size & 1U) != 0U
                   ? static_cast<std::size_t>((instruction >> 30) & 1U)
                   : (static_cast<std::size_t>((instruction >> 30) & 1U) << 1) |
                         scale_bit;
      }
      lane_offset = lane * element_size;
    }
  }

  const auto transfer_size =
      single ? element_size * register_count : vector_size * register_count;
  std::array<std::byte, 64> bytes{};
  auto transfer = std::span{bytes}.first(transfer_size);
  const bool base_sp = base_register == 31U;
  const bool tagchecked = post_index || !base_sp;
  if (load) {
    if (uml_arm64_simd_structure_memory_transfer(
            cpu, tlb, tagged_address, transfer.data(), transfer.size(), 0,
            base_sp,
            static_cast<std::uint32_t>(
                (element_size << 8) | static_cast<std::size_t>(tagchecked))) ==
        0U)
      return pack_jit_result(Event::memory_fault, 0);
    for (unsigned index = 0; index < register_count; ++index) {
      auto &destination = cpu->vectors[(first_register + index) & 31U].bytes;
      if (!single) {
        destination.fill(std::byte{});
        if (interleaved) {
          for (std::size_t lane = 0; lane < vector_size / element_size; ++lane)
            std::memcpy(destination.data() + lane * element_size,
                        bytes.data() +
                            (lane * register_count + index) * element_size,
                        element_size);
        } else {
          std::memcpy(destination.data(), bytes.data() + index * vector_size,
                      vector_size);
        }
      } else if (replicate) {
        destination.fill(std::byte{});
        for (std::size_t offset = 0; offset < vector_size;
             offset += element_size)
          std::memcpy(destination.data() + offset,
                      bytes.data() + index * element_size, element_size);
      } else {
        std::memcpy(destination.data() + lane_offset,
                    bytes.data() + index * element_size, element_size);
      }
    }
  } else {
    for (unsigned index = 0; index < register_count; ++index) {
      const auto &source = cpu->vectors[(first_register + index) & 31U].bytes;
      if (!single) {
        if (interleaved) {
          for (std::size_t lane = 0; lane < vector_size / element_size; ++lane)
            std::memcpy(bytes.data() +
                            (lane * register_count + index) * element_size,
                        source.data() + lane * element_size, element_size);
        } else {
          std::memcpy(bytes.data() + index * vector_size, source.data(),
                      vector_size);
        }
      } else {
        std::memcpy(bytes.data() + index * element_size,
                    source.data() + lane_offset, element_size);
      }
    }
    if (uml_arm64_simd_structure_memory_transfer(
            cpu, tlb, tagged_address, transfer.data(), transfer.size(), 1,
            base_sp,
            static_cast<std::uint32_t>(
                (element_size << 8) | static_cast<std::size_t>(tagchecked))) ==
        0U)
      return pack_jit_result(Event::memory_fault, 0);
  }
  if (post_index) {
    const auto offset = offset_register == 31U
                            ? transfer_size
                            : cpu->read_register(offset_register, false);
    cpu->write_register(base_register, tagged_address + offset, true);
  }
  cpu->pc += sizeof(instruction);
  return pack_jit_result(Event::continued, 1);
}

} // namespace
} // namespace uml::cpu::arm64
