// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The JIT's non-streaming SVE profile at jit::sve_support = complete, form
 * by form against the engine-neutral semantic library
 * (cpu/arm64/detail/sve_semantics.hpp): every row of the encoding table the
 * virtual CPU implements, whether the translation lowers it inline or sends
 * it to the library, runs as random words on random state through the JIT
 * and through execute_sve() on a copy of that state, and leaves the same
 * registers, predicates, FFR, NZCV and FPSR, the same event, fault and
 * syndrome, and the same memory.
 *
 * Each word runs alone in a block and again behind an FP instruction that
 * leaves the guest's FP environment active in the host's registers. A
 * memory word runs from bases inside a page, at a page's end, across into
 * the unmapped page after the two, and tagged under TBI. A floating-point
 * word also runs with FPCR trap enables set, where the executor runs it
 * through the library itself and the library decides the trap.
 *
 * No host implements SVE, so the SVE profile is injected into the
 * capabilities the executor translates for.
 */
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/detail/sve_semantics.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_sve_support.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/arm64/sve_profile.hpp"
#include "cpu/host_capabilities.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

namespace {

namespace arm64 = uml::cpu::arm64;
using arm64::JitExecutor;
using uml::cpu::ArmFeature;
using uml::cpu::HostCapabilities;
using uml::emulator::tests::jit_available;
using uml::emulator::tests::JitBacking;
using uml::emulator::tests::JitMapping;
using uml::memory::StaticGuestMemory;
using uml::testing::require;
using Event = uml::cpu::EngineEvent;
using Bytes = std::array<std::uint8_t, 16>;

constexpr std::uint64_t code_base = 0x94'0000;
constexpr std::size_t page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr std::size_t mapped_pages = 2;
constexpr std::size_t mapped = mapped_pages * page;
constexpr std::uint32_t svc = 0xd400'0001U;
/* fadd s31, s31, s31 on 1.0: exact, so it raises nothing, and it leaves
 * the guest's FP environment active in the host's registers. */
constexpr std::uint32_t activate = 0x1e3f'2bffU;
/* The words each sample's program occupies. */
constexpr std::size_t slot_bytes = 32;
constexpr std::size_t slots = 40;

/* The probed host without SME and with every SVE token injected, plus the
 * BF16 and I8MM tokens their SVE forms need; the library implements
 * those forms whatever the host does. */
[[nodiscard]] HostCapabilities sve_host() {
  auto host = uml::cpu::host_capabilities();
  for (const auto feature : {ArmFeature::sme, ArmFeature::sme2,
                             ArmFeature::sme_f64f64, ArmFeature::sme_i16i64})
    host.supported.reset(static_cast<std::size_t>(feature));
  host.sme_svl_bytes = 0;
  host.sme_hardware = false;
  for (const auto feature : arm64::sve_features)
    host.supported.set(static_cast<std::size_t>(feature));
  for (const auto feature : {ArmFeature::bf16, ArmFeature::i8mm})
    host.supported.set(static_cast<std::size_t>(feature));
  return host;
}

class Random final {
public:
  explicit Random(std::uint64_t seed) noexcept : state_{seed} {}

  std::uint64_t next() noexcept {
    state_ += 0x9e37'79b9'7f4a'7c15ULL;
    auto value = state_;
    value = (value ^ (value >> 30)) * 0xbf58'476d'1ce4'e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d0'49bb'1331'11ebULL;
    return value ^ (value >> 31);
  }

  std::uint16_t predicate() noexcept {
    /* Mostly random, sometimes empty, full or one run. */
    switch (next() % 8U) {
    case 0:
      return 0;
    case 1:
      return 0xffff;
    case 2:
      return static_cast<std::uint16_t>((1U << (next() % 17U)) - 1U);
    default:
      return static_cast<std::uint16_t>(next());
    }
  }

  Bytes bytes() noexcept {
    Bytes result{};
    const auto low = next();
    const auto high = next();
    std::memcpy(result.data(), &low, 8);
    std::memcpy(result.data() + 8, &high, 8);
    return result;
  }

private:
  std::uint64_t state_;
};

/* Lanes that exercise the floating-point and integer paths: values near
 * 1.0 so sums and products stay finite and round, the specials (zeros,
 * infinities, NaNs of both kinds, denormals, the extremes), small
 * integers and byte extremes, or raw bits. */
void fill_lanes(Random &random, Bytes &bytes) {
  const auto kind = random.next() % 8U;
  if (kind == 0U) {
    bytes = random.bytes();
    return;
  }
  static constexpr unsigned sizes[]{2, 2, 4, 8, 1, 2, 4, 8};
  const unsigned esize = sizes[kind - 1U];
  for (unsigned index = 0; index < 16U / esize; ++index) {
    std::uint64_t value = random.next();
    switch (kind) {
    case 1: /* FP16 with exponent 13-17. */
      value = (value & 0x83ffU) | ((13U + value % 5U) << 10);
      break;
    case 2: /* FP16 integers and extremes. */
      value = std::array<std::uint64_t, 6>{0x0000U, 0x8000U, 0x3c00U,
                                           0x7bffU, 0x0001U, 0xc500U}[value % 6U];
      break;
    case 3: /* FP32 with exponent 124-130. */
      value = (value & 0x807f'ffffU) | ((124U + value % 7U) << 23);
      break;
    case 4: /* FP64 with exponent 1020-1026. */
      value = (value & 0x800f'ffff'ffff'ffffULL) | ((1020ULL + value % 7U) << 52);
      break;
    case 5: /* Small integers and byte extremes. */
      value = std::array<std::uint64_t, 6>{0x00U, 0x01U, 0x7fU,
                                           0x80U, 0xffU, value % 9U}[value % 6U];
      break;
    case 6: /* FP16 and FP32 specials. */
      if (esize == 2U)
        value = std::array<std::uint64_t, 10>{
            0x0000U, 0x8000U, 0x7c00U, 0xfc00U, 0x7e00U,
            0x7d00U, 0x0001U, 0x03ffU, 0x7bffU, 0xfbffU}[value % 10U];
      else
        value = std::array<std::uint64_t, 10>{
            0x0000'0000U, 0x8000'0000U, 0x7f80'0000U, 0xff80'0000U,
            0x7fc0'0000U, 0x7fa0'0000U, 0x0000'0001U, 0x007f'ffffU,
            0x7f7f'ffffU, 0xff7f'ffffU}[value % 10U];
      break;
    default: /* FP64 specials. */
      value = std::array<std::uint64_t, 10>{
          0x0000'0000'0000'0000ULL, 0x8000'0000'0000'0000ULL,
          0x7ff0'0000'0000'0000ULL, 0xfff0'0000'0000'0000ULL,
          0x7ff8'0000'0000'0000ULL, 0x7ff4'0000'0000'0000ULL,
          0x0000'0000'0000'0001ULL, 0x000f'ffff'ffff'ffffULL,
          0x7fef'ffff'ffff'ffffULL, 0xffef'ffff'ffff'ffffULL}[value % 10U];
      break;
    }
    for (unsigned byte = 0; byte < esize; ++byte)
      bytes[index * esize + byte] =
          static_cast<std::uint8_t>(value >> (8U * byte));
  }
}

/* Random architectural state for a differential run. FPCR takes a random
 * rounding mode, FZ, DN and FZ16 and, with `traps`, a random trap enable
 * set. */
[[nodiscard]] arm64::CpuState random_state(Random &random, bool traps) {
  arm64::CpuState cpu{};
  for (auto &value : cpu.registers) {
    const auto raw = random.next();
    switch (raw % 4U) {
    case 0:
      value = raw >> 58;
      break;
    case 1:
      value = ~(raw >> 58);
      break;
    case 2:
      value = (raw & 1U) != 0U ? 0x7fff'ffff'ffff'ffffULL - (raw >> 60)
                               : 0x8000'0000'0000'0000ULL + (raw >> 60);
      break;
    default:
      value = raw;
      break;
    }
  }
  cpu.sp = random.next() & ~std::uint64_t{15};
  for (unsigned n = 0; n < 32; ++n) {
    Bytes bytes{};
    fill_lanes(random, bytes);
    std::memcpy(cpu.vectors[n].bytes.data(), bytes.data(), 16);
  }
  for (unsigned n = 0; n < 16; ++n)
    cpu.sve_p[n] = arm64::sve_predicate_mask(random.predicate());
  cpu.sve_ffr = arm64::sve_predicate_mask(random.predicate());
  cpu.sve_live = static_cast<std::uint8_t>(random.next() & 1U);
  cpu.nzcv = static_cast<std::uint32_t>(random.next() & 0xfU) << 28;
  cpu.fpcr = static_cast<std::uint32_t>(random.next()) &
             ((3U << 22) | (1U << 24) | (1U << 25) | (1U << 19));
  if (traps)
    cpu.fpcr |= static_cast<std::uint32_t>(random.next() & 0x9fU) << 8;
  cpu.fpsr = static_cast<std::uint32_t>(random.next()) & 0x0800'009fU;
  return cpu;
}

/* The machine: one executor on one host profile, code in slots at the
 * start of the first page, data in the rest of the two mapped pages. */
class Machine final {
public:
  explicit Machine(const HostCapabilities &host)
      : generation_{next_generation_++}, backing_{mapped_pages + 1},
        mapping_{generation_, code_base, backing_, mapped_pages},
        executor_{arm64::JitExecutorOptions{.host_capabilities = host}} {}

  Machine(const Machine &) = delete;
  Machine &operator=(const Machine &) = delete;

  void put(std::size_t offset, std::span<const std::byte> bytes) {
    backing_.put(offset, bytes);
  }
  void get(std::size_t offset, std::span<std::byte> bytes) const {
    backing_.get(offset, bytes);
  }

  /* Runs the program at `pc` until an exit other than a continued block. */
  Event run(std::uint64_t pc, std::size_t &retired) {
    cpu.pc = pc;
    require(executor_.begin_execution(generation_, pc));
    Event event = Event::fatal;
    retired = 0;
    for (std::size_t block = 0; block < 64U; ++block) {
      std::size_t executed = 0;
      event = executor_.run_block(cpu, memory_, generation_, executed);
      retired += executed;
      if (event != Event::continued)
        break;
    }
    executor_.end_execution();
    arm64::restore_native_fp_environment(cpu);
    return event;
  }

  [[nodiscard]] std::uint64_t fault_address() const {
    return executor_.fault_address();
  }
  [[nodiscard]] bool fault_alignment() const {
    return executor_.fault_alignment();
  }
  [[nodiscard]] std::uint32_t floating_point_syndrome() const {
    return executor_.floating_point_syndrome();
  }

  arm64::CpuState cpu{};

private:
  static inline unsigned long next_generation_ = 0x6100;
  unsigned long generation_;
  JitBacking backing_;
  JitMapping mapping_;
  JitExecutor executor_;
  StaticGuestMemory memory_;
};

/* Guest memory for a reference run of the library: the machine's mapped
 * pages, copied. */
class CopiedMemory final : public arm64::detail::SveMemory {
public:
  explicit CopiedMemory(std::vector<std::byte> &bytes) : bytes_{bytes} {}

  std::byte *translate(std::uint64_t address,
                       uml::memory::MemoryAccess) noexcept override {
    if (address < code_base || address - code_base >= bytes_.size())
      return nullptr;
    return bytes_.data() + (address - code_base);
  }
  void stored(std::byte *, std::size_t) noexcept override {}

private:
  std::vector<std::byte> &bytes_;
};

struct Tally final {
  std::size_t runs{};
  std::size_t rows{};
  std::size_t faults{};
  std::size_t traps{};
  std::size_t inline_rows{};
};

void print_bytes(const char *label, std::span<const std::byte> bytes) {
  std::fprintf(stderr, "  %s ", label);
  for (const auto byte : bytes)
    std::fprintf(stderr, "%02x", std::to_integer<unsigned>(byte));
  std::fprintf(stderr, "\n");
}

/* Runs `word`, behind an activating FP instruction when `behind`, on the
 * JIT from `initial` over `data`, and against the library on a copy; any
 * difference is reported and fails. */
void compare(Machine &machine, std::size_t slot, std::uint32_t word,
             std::size_t index, bool behind, const arm64::CpuState &initial,
             const std::vector<std::byte> &data, bool traps, Tally &tally) {
  const auto &row = arm64::detail::sme_encodings[index];
  const std::array alone{word, svc};
  const std::array activated{activate, word, svc};
  const std::span<const std::uint32_t> code =
      behind ? std::span<const std::uint32_t>{activated}
             : std::span<const std::uint32_t>{alone};
  /* A program per slot and variant, so no block is ever reused after the
   * words under it changed. */
  const auto offset = (slot * 2U + (behind ? 1U : 0U)) * slot_bytes;
  std::vector<std::byte> before = data;
  std::memcpy(before.data() + offset, code.data(), code.size_bytes());
  machine.put(0, before);
  machine.cpu = initial;
  std::size_t retired = 0;
  const auto event = machine.run(code_base + offset, retired);
  std::vector<std::byte> after(mapped);
  machine.get(0, after);

  /* Beyond the FP instruction, the library's view of the state. */
  auto expected = initial;
  if (behind) {
    /* fadd s31, s31, s31 on 1.0 leaves 2.0 in S31 and clears the rest of
     * the register; it raises nothing. */
    expected.vectors[31].bytes = {};
    const auto two = std::bit_cast<std::uint32_t>(2.0F);
    std::memcpy(expected.vectors[31].bytes.data(), &two, 4);
  }
  CopiedMemory reference{before};
  const auto result = arm64::detail::execute_sve(expected, word, row, reference);
  const bool faulted = result.event == arm64::detail::SveEvent::memory_fault;
  const bool trapped =
      result.event == arm64::detail::SveEvent::floating_point_trap;
  tally.faults += faulted ? 1U : 0U;
  tally.traps += trapped ? 1U : 0U;
  const auto at = code_base + offset + (behind ? 4U : 0U);
  const Event want = faulted   ? Event::memory_fault
                     : trapped ? Event::floating_point_trap
                               : Event::system_call;
  bool same = result.event != arm64::detail::SveEvent::fatal &&
              result.event != arm64::detail::SveEvent::undefined &&
              event == want &&
              (!faulted || (machine.cpu.pc == at &&
                            machine.fault_address() == result.fault_address &&
                            machine.fault_alignment() == result.alignment)) &&
              (!trapped || (machine.cpu.pc == at &&
                            machine.floating_point_syndrome() ==
                                result.syndrome)) &&
              (faulted || trapped || retired == code.size()) &&
              machine.cpu.registers == expected.registers &&
              machine.cpu.sp == expected.sp &&
              machine.cpu.sve_p == expected.sve_p &&
              machine.cpu.sve_ffr == expected.sve_ffr &&
              machine.cpu.nzcv == expected.nzcv &&
              machine.cpu.fpsr == expected.fpsr &&
              machine.cpu.fpcr == expected.fpcr &&
              machine.cpu.sve_live == expected.sve_live && after == before;
  unsigned differing = 32;
  for (unsigned n = 0; n < 32 && same; ++n)
    if (machine.cpu.vectors[n].bytes != expected.vectors[n].bytes) {
      differing = n;
      same = false;
    }
  if (same) {
    ++tally.runs;
    return;
  }
  std::fprintf(stderr,
               "word %08x (row %zu, SVE-%04u%s%s): the JIT leaves another "
               "state than the library\n  event %d (want %d, library %d), "
               "retired %zu, pc %#llx/%#llx, fault %#llx/%#llx, nzcv "
               "%08x/%08x, fpsr %08x/%08x, fpcr %08x, memory %s\n",
               word, index, row.form & 0x7fffU, behind ? ", FP active" : "",
               traps ? ", traps" : "", static_cast<int>(event),
               static_cast<int>(want), static_cast<int>(result.event), retired,
               static_cast<unsigned long long>(machine.cpu.pc),
               static_cast<unsigned long long>(at),
               static_cast<unsigned long long>(machine.fault_address()),
               static_cast<unsigned long long>(result.fault_address),
               machine.cpu.nzcv, expected.nzcv, machine.cpu.fpsr, expected.fpsr,
               initial.fpcr, after == before ? "same" : "differs");
  if (differing < 32U) {
    print_bytes("jit    ", machine.cpu.vectors[differing].bytes);
    print_bytes("library", expected.vectors[differing].bytes);
    std::fprintf(stderr, "  in Z%u\n", differing);
  }
  if (machine.cpu.sve_p != expected.sve_p)
    std::fprintf(stderr, "  P differs\n");
  require(false);
}

/* Whether a base register of `row` is the stack pointer (Rn = 31). */
void place_base(const arm64::detail::SmeEncoding &row, std::uint32_t word,
                Random &random, arm64::CpuState &initial) {
  const auto &memory = arm64::detail::sme_encoding_memory[row.memory - 1U];
  const auto *const operands = arm64::detail::sme_encoding_operands_of(row);
  /* The base: mostly inside the second page, sometimes at the end of a
   * page, or unmapped. */
  std::uint64_t base = code_base + page + 64U + random.next() % (page - 192U);
  switch (random.next() % 8U) {
  case 0:
    base = code_base + page - random.next() % 48U;
    break;
  case 1:
    base = code_base + mapped - random.next() % 48U;
    break;
  case 2:
    base = code_base + mapped + random.next() % page;
    break;
  default:
    break;
  }
  const auto base_register =
      arm64::detail::sme_operand_number(operands[memory.base_operand], word);
  if (memory.index_operand != arm64::detail::sme_no_operand) {
    const auto index_register =
        arm64::detail::sme_operand_number(operands[memory.index_operand], word);
    initial.registers[index_register] = random.next() % 4U;
  }
  if (random.next() % 4U == 0U) {
    initial.tbi_enabled = true;
    base |= (random.next() & 0xffU) << 56;
  }
  if (base_register == 31U)
    initial.sp = base & ~std::uint64_t{15};
  else
    initial.registers[base_register] = base;
}

/* Every row the virtual CPU implements agrees with the library. */
void forms_match_the_library(const HostCapabilities &host, Tally &tally) {
  Random random{0x6a17'5e01U};
  const auto features = arm64::detail::implemented_sve_features(host);
  for (std::size_t index = 0; index < arm64::detail::sme_encodings.size();
       ++index) {
    const auto &row = arm64::detail::sme_encodings[index];
    if ((row.non_streaming_requirement & arm64::detail::sme_feature_sve) == 0U ||
        (row.non_streaming_requirement & features) !=
            row.non_streaming_requirement ||
        (row.mode & arm64::detail::sme_mode_za) != 0U)
      continue;
    const bool memory = row.memory != 0U;
    if (arm64::jit::implements_sve_form(row.form))
      ++tally.inline_rows;
    ++tally.rows;
    Machine machine{host};
    const std::size_t samples = memory ? 14U : slots;
    for (std::size_t slot = 0; slot < samples; ++slot) {
      const auto word = row.value | (static_cast<std::uint32_t>(random.next()) &
                                     ~row.mask);
      const auto *decoded = arm64::detail::decode_sve_non_streaming(word, features);
      if (decoded != &row || arm64::jit::sve_word_reserved(row.form, word))
        continue;
      std::vector<std::byte> data(mapped);
      for (std::size_t offset = 0; offset < mapped; offset += 8) {
        const auto value = random.next();
        std::memcpy(data.data() + offset, &value, 8);
      }
      const bool traps = !memory && random.next() % 4U == 0U;
      auto initial = random_state(random, traps);
      if (memory) {
        for (unsigned n = 0; n < 16; ++n)
          if (random.next() % 2U == 0U)
            initial.sve_p[n] = arm64::sve_predicate_mask(0xffffU);
        place_base(row, word, random, initial);
      }
      /* S31 must hold 1.0 for the activating FADD to be exact. */
      initial.vectors[31].bytes = {};
      const auto one = std::bit_cast<std::uint32_t>(1.0F);
      for (const bool behind : {false, true}) {
        auto start = initial;
        if (behind) {
          std::memcpy(start.vectors[31].bytes.data(), &one, 4);
          /* The FADD's own result is part of the expectation. */
        }
        compare(machine, slot, word, index, behind, start, data, traps, tally);
      }
    }
  }
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  if (!jit_available()) {
    std::printf("skipped: the JIT does not run here\n");
    return 0;
  }
  require(arm64::jit::sve_support == arm64::SveSupport::complete);
  Tally tally;
  forms_match_the_library(sve_host(), tally);
  require(tally.rows > 400U && tally.runs > tally.rows && tally.faults > 0U &&
          tally.traps > 0U);
  std::printf("%zu runs over %zu rows (%zu inline) match the library, %zu of "
              "them faulting and %zu trapping\n",
              tally.runs, tally.rows, tally.inline_rows, tally.faults,
              tally.traps);
  return 0;
}
