// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The JIT's C++ side of non-streaming SVE: the words no inline lowering
 * covers run through the engine-neutral semantic library
 * (cpu/arm64/detail/sve_semantics.hpp) on the register file. Generated code
 * reaches it as a terminal helper, whose spill has filled CpuState; the
 * executor's trapped-exception mode reaches the same function directly, so
 * the library alone decides a floating-point form's trap and the flags it
 * commits (detail::decide_floating_exception).
 */
#include "jit_semantic_memory.hpp"
#include "jit_semantic_reservation.hpp"
#include "jit_semantic_sve.hpp"
#include "jit_sve_support.hpp"

#include "cpu/arm64/fp_environment.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <span>

namespace uml::cpu::arm64 {

namespace {

/* Guest memory through the executor's TLB. The library forms every
 * address untagged, so the plain translation applies; a miss there records
 * its fault in the TLB, which a first-fault or non-fault load's probe may
 * leave behind, and execute_sve_word replaces that record with the
 * library's own whenever the instruction faults. A store invalidates the
 * software reservations of the host bytes it wrote as it writes them. */
class JitSveMemory final : public detail::SveMemory {
public:
  explicit JitSveMemory(Tlb &tlb) noexcept : tlb_{tlb} {}

  std::byte *translate(std::uint64_t address,
                       ::uml::memory::MemoryAccess access) noexcept override {
    return tlb_.translate(address, ::uml::memory::translated_access(access));
  }

  void stored(std::byte *host, std::size_t size) noexcept override {
    const JitStoreRange range{reinterpret_cast<std::uintptr_t>(host), size};
    jit::semantic::invalidate_store_ranges(std::span{&range, 1});
  }

private:
  Tlb &tlb_;
};

} // namespace

namespace jit::semantic {

namespace {

/* The words run through the library per table row, and the last of them,
 * while counting. */
std::array<std::atomic<std::uint64_t>, detail::sme_encodings.size()>
    library_exits{};
std::array<std::atomic<std::uint32_t>, detail::sme_encodings.size()>
    library_exit_words{};
std::atomic<bool> counting{false};

} // namespace

void count_sve_library_exits(bool enable) noexcept {
  counting.store(enable, std::memory_order_relaxed);
}

std::uint64_t sve_library_exits(std::size_t row) noexcept {
  return row < library_exits.size()
             ? library_exits[row].load(std::memory_order_relaxed)
             : 0U;
}

std::uint32_t sve_library_exit_word(std::size_t row) noexcept {
  return row < library_exit_words.size()
             ? library_exit_words[row].load(std::memory_order_relaxed)
             : 0U;
}

std::uint64_t sve_library_exit_total() noexcept {
  std::uint64_t total = 0;
  for (const auto &count : library_exits)
    total += count.load(std::memory_order_relaxed);
  return total;
}

void reset_sve_library_exits() noexcept {
  for (auto &count : library_exits)
    count.store(0, std::memory_order_relaxed);
  for (auto &word : library_exit_words)
    word.store(0, std::memory_order_relaxed);
}

sve_detail::SveResult execute_sve_word(CpuState &cpu, Tlb &tlb,
                                   std::uint32_t word,
                                   const sve_detail::SmeEncoding &row) noexcept {
  if (counting.load(std::memory_order_relaxed)) {
    const auto index =
        static_cast<std::size_t>(&row - detail::sme_encodings.data());
    if (index < library_exits.size()) {
      library_exits[index].fetch_add(1, std::memory_order_relaxed);
      library_exit_words[index].store(word, std::memory_order_relaxed);
    }
  }
  JitSveMemory memory{tlb};
  const auto result = sve_detail::execute_sve(cpu, word, row, memory);
  if (result.event == sve_detail::SveEvent::memory_fault) {
    tlb.fault_address = result.fault_address;
    tlb.fault_access = ::uml::memory::translated_access(result.access);
    tlb.fault_alignment = result.alignment ? 1U : 0U;
  }
  return result;
}

std::optional<SveWordRun> run_sve_word(
    CpuState &cpu, Tlb &tlb, std::uint32_t word,
    const ::uml::cpu::HostCapabilities &host) noexcept {
  const auto *row = sve_detail::decode_sve_non_streaming(
      word, sve_detail::implemented_sve_features(host));
  if (row == nullptr ||
      ((row->mode & sve_detail::sme_mode_za) != 0U &&
       (cpu.svcr & svcr_za) == 0U) ||
      !sve_detail::sve_semantics_covers(*row))
    return std::nullopt;
  if (jit::sve_word_reserved(row->form, word))
    return SveWordRun{Event::illegal_instruction, 0, 0};
  restore_native_fp_environment(cpu);
  const auto result = execute_sve_word(cpu, tlb, word, *row);
  switch (result.event) {
  case sve_detail::SveEvent::retired:
    cpu.pc += sizeof(std::uint32_t);
    return SveWordRun{Event::continued, 1, 0};
  case sve_detail::SveEvent::memory_fault:
    return SveWordRun{Event::memory_fault, 0, 0};
  case sve_detail::SveEvent::floating_point_trap:
    return SveWordRun{Event::floating_point_trap, 0, result.syndrome};
  case sve_detail::SveEvent::undefined:
    return SveWordRun{Event::illegal_instruction, 0, 0};
  case sve_detail::SveEvent::fatal:
    break;
  }
  return SveWordRun{Event::fatal, 0, 0};
}

} // namespace jit::semantic

/* The slow path of every SM=0 SVE word the translation does not lower: a
 * terminal helper, so the block's spill has filled CpuState. A trapped
 * floating-point form never gets here, since the executor runs each word
 * through the library itself while an FPCR trap enable is set. */
extern "C" std::uint64_t
uml_arm64_jit_execute_sve(CpuState *cpu, Tlb *tlb,
                          std::uint32_t instruction) noexcept {
  const auto *row = detail::find_sme_encoding(instruction);
  if (cpu == nullptr || tlb == nullptr || row == nullptr ||
      !detail::sve_semantics_covers(*row))
    return pack_jit_result(Event::fatal, 0);
  restore_native_fp_environment(*cpu);
  switch (jit::semantic::execute_sve_word(*cpu, *tlb, instruction, *row)
              .event) {
  case detail::SveEvent::retired:
    cpu->pc += sizeof(std::uint32_t);
    return pack_jit_result(Event::continued, 1);
  case detail::SveEvent::memory_fault:
    return pack_jit_result(Event::memory_fault, 0);
  case detail::SveEvent::undefined:
    return pack_jit_result(Event::illegal_instruction, 0);
  case detail::SveEvent::floating_point_trap:
  case detail::SveEvent::fatal:
    break;
  }
  return pack_jit_result(Event::fatal, 0);
}

} // namespace uml::cpu::arm64
