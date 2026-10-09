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
 * Each word runs alone in a block, again behind an FP instruction that
 * leaves the guest's FP environment active in the host's registers, and
 * again behind PTRUE of P0-P7, where the translation knows every governing
 * predicate all-active and emits no check. A
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
#include "cpu/arm64/floating_controls.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_publication.hpp"
#include "cpu/arm64/jit/jit_semantic_sve.hpp"
#include "cpu/arm64/jit/jit_sve_support.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
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
namespace jit_test = arm64::jit::testing;
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
constexpr std::size_t mapped_pages = 4;
constexpr std::size_t mapped = mapped_pages * page;
constexpr std::uint32_t svc = 0xd400'0001U;
constexpr std::uint32_t nop = 0xd503'201fU;
/* fadd s31, s31, s31 on 1.0: exact, so it raises nothing, and it leaves
 * the guest's FP environment active in the host's registers. */
constexpr std::uint32_t activate = 0x1e3f'2bffU;
/* The words each sample's program occupies. */
constexpr std::size_t slot_bytes = 48;
constexpr std::size_t slots = 28;
constexpr std::size_t variants = 3;
/* ptrue p<n>.b, all. */
constexpr std::uint32_t ptrue_b = 0x2518'e3e0U;

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
 * rounding mode, FZ, DN and FZ16, FIZ, AH and NEP where the host implements
 * FEAT_AFP and, with `traps`, a random trap enable set. */
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
  /* FIZ, AH and NEP are guest state only where the virtual CPU implements
   * FEAT_AFP, which the probed host decides here. */
  static const bool afp = arm64::implements_afp(sve_host());
  if (afp)
    cpu.fpcr |= static_cast<std::uint32_t>(random.next()) &
                arm64::fpcr_afp_controls;
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

/* How a word is preceded in its block. */
enum class Variant { alone, behind_fp, behind_ptrue };

/* Runs `word` after the variant's prefix on the JIT from `initial` over
 * `data`, and against the library on a copy; any difference is reported
 * and fails. */
void compare(Machine &machine, std::size_t slot, std::uint32_t word,
             std::size_t index, Variant variant,
             const arm64::CpuState &initial, const std::vector<std::byte> &data,
             bool traps, Tally &tally) {
  const auto &row = arm64::detail::sme_encodings[index];
  std::vector<std::uint32_t> code;
  if (variant == Variant::behind_fp)
    code.push_back(activate);
  if (variant == Variant::behind_ptrue)
    for (std::uint32_t n = 0; n < 8U; ++n)
      code.push_back(ptrue_b | n);
  const auto prefix = code.size();
  code.push_back(word);
  code.push_back(svc);
  const bool behind = variant == Variant::behind_fp;
  /* A program per slot and variant, so no block is ever reused after the
   * words under it changed. */
  const auto offset =
      (slot * variants + static_cast<std::size_t>(variant)) * slot_bytes;
  std::vector<std::byte> before = data;
  std::memcpy(before.data() + offset, code.data(),
              code.size() * sizeof(std::uint32_t));
  machine.put(0, before);
  machine.cpu = initial;
  std::size_t retired = 0;
  const auto event = machine.run(code_base + offset, retired);
  std::vector<std::byte> after(mapped);
  machine.get(0, after);

  /* Beyond the FP instruction, the library's view of the state. */
  auto expected = initial;
  if (variant == Variant::behind_ptrue)
    for (unsigned n = 0; n < 8U; ++n)
      expected.sve_p[n] = arm64::sve_predicate_mask(0xffffU);
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
  const auto at = code_base + offset + prefix * 4U;
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
               word, index, row.form & 0x7fffU, behind ? ", FP active"
                               : prefix != 0U ? ", P0-P7 true" : "",
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
  if (differing < 32U || machine.cpu.fpsr != expected.fpsr) {
    /* The words' conventional operand fields, before the run. */
    for (const unsigned low : {0U, 5U, 16U}) {
      const unsigned n = (word >> low) & 31U;
      std::fprintf(stderr, "  initial Z%u (bits %u)", n, low);
      print_bytes("", initial.vectors[n].bytes);
    }
  }
  for (unsigned n = 0; n < machine.cpu.registers.size(); ++n)
    if (machine.cpu.registers[n] != expected.registers[n])
      std::fprintf(stderr, "  X%u: jit %#llx, library %#llx (was %#llx)\n", n,
                   static_cast<unsigned long long>(machine.cpu.registers[n]),
                   static_cast<unsigned long long>(expected.registers[n]),
                   static_cast<unsigned long long>(initial.registers[n]));
  if (machine.cpu.sp != expected.sp)
    std::fprintf(stderr, "  SP: jit %#llx, library %#llx\n",
                 static_cast<unsigned long long>(machine.cpu.sp),
                 static_cast<unsigned long long>(expected.sp));
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
  const bool vector_base =
      operands[memory.base_operand].kind == arm64::detail::SmeOperandKind::z;
  const auto base_register =
      arm64::detail::sme_operand_number(operands[memory.base_operand], word);
  if (memory.index_operand != arm64::detail::sme_no_operand &&
      operands[memory.index_operand].kind != arm64::detail::SmeOperandKind::z) {
    const auto index_register =
        arm64::detail::sme_operand_number(operands[memory.index_operand], word);
    if (index_register != 31U)
      initial.registers[index_register] = random.next() % 4U;
  }
  if (random.next() % 4U == 0U) {
    initial.tbi_enabled = true;
    base |= (random.next() & 0xffU) << 56;
  }
  if (vector_base)
    return;
  if (base_register == 31U)
    initial.sp = base & ~std::uint64_t{15};
  else
    initial.registers[base_register] = base;
}

/* The vector operands of a gather or scatter: each element's base address or
 * offset. `clean` keeps every access inside one mapped page; otherwise a lane
 * is sometimes at a page's end, unmapped or random, and a 32-bit offset gets
 * junk in its element's high word, which the form must ignore. */
void place_vectors(const arm64::detail::SmeEncoding &row, std::uint32_t word,
                   Random &random, arm64::CpuState &initial, bool clean) {
  const auto &memory = arm64::detail::sme_encoding_memory[row.memory - 1U];
  if (memory.shape != arm64::detail::SmeMemoryShape::gather &&
      memory.shape != arm64::detail::SmeMemoryShape::gather_first_fault)
    return;
  const auto *const operands = arm64::detail::sme_encoding_operands_of(row);
  const unsigned esize = arm64::detail::sme_memory_esize(memory, word) / 8U;
  const bool vector_base =
      operands[memory.base_operand].kind == arm64::detail::SmeOperandKind::z;
  const bool vector_index = memory.offset == arm64::detail::SmeAddressOffset::vector_index;
  const bool signed_index =
      vector_index && memory.index_bits < 64U &&
      arm64::detail::sme_segment_value(memory.index_sign, word) != 0U;
  const auto place = [&](unsigned number, bool address) {
    auto &vector = initial.vectors[number & 31U];
    for (unsigned lane = 0; lane < 16U / esize; ++lane) {
      /* Each lane draws its own case, so an access in the middle of a
       * scatter can be the one that faults. */
      const unsigned mode =
          clean ? 0U : static_cast<unsigned>(random.next() % 8U);
      std::uint64_t value;
      if (address) {
        value = code_base + mapped / 2U + page / 2U + random.next() % 256U;
        if (mode == 1U)
          value = code_base + 2U * page - random.next() % 8U;
        else if (mode == 2U)
          value = code_base + mapped + random.next() % page;
        else if (mode == 3U)
          value = random.next();
        if (!clean && initial.tbi_enabled)
          value |= (random.next() & 0xffU) << 56;
        if (esize == 4U)
          value &= 0xffff'ffffU;
      } else {
        /* A small offset, negative where the form sign-extends it. */
        std::int64_t small = static_cast<std::int64_t>(random.next() % 32U);
        if ((signed_index || memory.index_bits == 64U) && random.next() % 2U == 0U)
          small = -small;
        value = static_cast<std::uint64_t>(small);
        if (mode == 1U)
          value = random.next() % (page * 2U);
        else if (mode == 2U)
          value = random.next();
        if (memory.index_bits == 32U && esize == 8U)
          value = (value & 0xffff'ffffU) |
                  (clean || mode == 0U ? random.next() << 32 : value & ~0xffff'ffffULL);
        if (esize == 4U)
          value &= 0xffff'ffffU;
      }
      std::memcpy(vector.bytes.data() + lane * esize, &value, esize);
    }
  };
  if (vector_base)
    place(arm64::detail::sme_operand_number(operands[memory.base_operand], word),
          true);
  if (vector_index)
    place(arm64::detail::sme_operand_number(operands[memory.index_operand], word),
          false);
}

/* A base and index that nothing sends to the library: a register base 16-byte
 * aligned in the middle of a page in the middle of the mapping, a 4 KiB reach
 * from either end of it, and a small index. */
bool place_clean_base(const arm64::detail::SmeEncoding &row, std::uint32_t word,
                      Random &random, arm64::CpuState &initial) {
  const auto &memory = arm64::detail::sme_encoding_memory[row.memory - 1U];
  const auto *const operands = arm64::detail::sme_encoding_operands_of(row);
  const std::uint64_t base =
      (code_base + mapped / 2U + page / 2U + random.next() % 64U) &
      ~std::uint64_t{15};
  unsigned base_register = 31U;
  if (operands[memory.base_operand].kind != arm64::detail::SmeOperandKind::z) {
    base_register =
        arm64::detail::sme_operand_number(operands[memory.base_operand], word);
    if (base_register == 31U)
      initial.sp = base;
    else
      initial.registers[base_register] = base;
  }
  if (memory.index_operand != arm64::detail::sme_no_operand &&
      operands[memory.index_operand].kind != arm64::detail::SmeOperandKind::z) {
    const auto index_register = arm64::detail::sme_operand_number(
        operands[memory.index_operand], word);
    /* One register named twice cannot be both a base and a small index. */
    if (index_register != 31U && index_register == base_register)
      return false;
    if (index_register != 31U)
      initial.registers[index_register] = random.next() % 4U;
  }
  return true;
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
    ++tally.rows;
    bool lowered = false;
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
      /* Governing predicates are often all-active, so the inline bodies
       * run as often as the library's. */
      for (unsigned n = 0; n < 16; ++n)
        if (random.next() % 2U == 0U)
          initial.sve_p[n] = arm64::sve_predicate_mask(0xffffU);
      if (memory) {
        place_base(row, word, random, initial);
        place_vectors(row, word, random, initial, false);
      }
      /* Whether this word runs inline: a library word ends its block, an
       * inline one is followed by the rest. */
      if (!lowered) {
        const std::array probe{word, nop, nop};
        const uml::cpu::HostCapabilitiesScope scope{host};
        lowered = jit_test::compile_block_in_mode(code_base, probe, 0,
                                                  jit_test::LinkShape::unlinked)
                      .code.guest_instructions > 1U;
      }
      /* S31 must hold 1.0 for the activating FADD to be exact. */
      initial.vectors[31].bytes = {};
      const auto one = std::bit_cast<std::uint32_t>(1.0F);
      for (const auto variant : {Variant::alone, Variant::behind_fp,
                                 Variant::behind_ptrue}) {
        auto start = initial;
        if (variant == Variant::behind_fp) {
          std::memcpy(start.vectors[31].bytes.data(), &one, 4);
          /* The FADD's own result is part of the expectation. */
        }
        compare(machine, slot, word, index, variant, start, data, traps,
                tally);
      }
    }
    if (lowered)
      ++tally.inline_rows;
  }
}

/* A word the translation lowers stays inline from a state in which nothing
 * asks for the library: every predicate and FFR all-active, no FPCR trap
 * enable, an untagged aligned base inside the page. The library-exit counter
 * of its row must not move; a lowering whose fast path always bailed would
 * match the library and still cost its call. */
void lowered_words_stay_inline(const HostCapabilities &host, Tally &tally) {
  Random random{0x6a17'5e03U};
  const auto features = arm64::detail::implemented_sve_features(host);
  arm64::jit::semantic::count_sve_library_exits(true);
  std::size_t checked = 0;
  for (std::size_t index = 0; index < arm64::detail::sme_encodings.size();
       ++index) {
    const auto &row = arm64::detail::sme_encodings[index];
    if ((row.non_streaming_requirement & arm64::detail::sme_feature_sve) == 0U ||
        (row.non_streaming_requirement & features) !=
            row.non_streaming_requirement ||
        (row.mode & arm64::detail::sme_mode_za) != 0U)
      continue;
    Machine machine{host};
    for (std::size_t slot = 0; slot < 10U; ++slot) {
      const auto word = row.value | (static_cast<std::uint32_t>(random.next()) &
                                     ~row.mask);
      const auto *decoded = arm64::detail::decode_sve_non_streaming(word, features);
      if (decoded != &row || arm64::jit::sve_word_reserved(row.form, word))
        continue;
      const std::array probe{word, nop, nop};
      bool lowered = false;
      {
        const uml::cpu::HostCapabilitiesScope scope{host};
        lowered = jit_test::compile_block_in_mode(
                      code_base, probe, 0, jit_test::LinkShape::unlinked)
                      .code.guest_instructions > 1U;
      }
      std::vector<std::byte> data(mapped);
      for (std::size_t offset = 0; offset < mapped; offset += 8) {
        const auto value = random.next();
        std::memcpy(data.data() + offset, &value, 8);
      }
      auto initial = random_state(random, false);
      for (auto &predicate : initial.sve_p)
        predicate = arm64::sve_predicate_mask(0xffffU);
      initial.sve_ffr = arm64::sve_predicate_mask(0xffffU);
      initial.tbi_enabled = false;
      initial.vectors[31].bytes = {};
      if (row.memory != 0U) {
        if (!place_clean_base(row, word, random, initial))
          continue;
        place_vectors(row, word, random, initial, true);
      }
      /* A gather's translations never call the bridge, so a page the TLB
       * lacks sends it to the library once, which fills the TLB. */
      compare(machine, slot, word, index, Variant::alone, initial, data, false,
              tally);
      const auto before = arm64::jit::semantic::sve_library_exits(index);
      compare(machine, slot, word, index, Variant::alone, initial, data, false,
              tally);
      const auto taken = arm64::jit::semantic::sve_library_exits(index) - before;
      if (lowered && taken != 0U) {
        std::fprintf(stderr,
                     "word %08x (row %zu, SVE-%04u, memory shape %d) was "
                     "lowered but took the library from a clean state\n",
                     word, index, row.form & 0x7fffU,
                     row.memory == 0U
                         ? -1
                         : static_cast<int>(arm64::detail::sme_encoding_memory
                                                [row.memory - 1U].shape));
        require(false);
      }
      checked += lowered ? 1U : 0U;
    }
  }
  /* The counters agree with one another: each row that took the library
   * kept a word that decodes to it, and the rows add up to the total. */
  std::uint64_t total = 0;
  std::size_t rows_taking_the_library = 0;
  for (std::size_t index = 0; index < arm64::detail::sme_encodings.size();
       ++index) {
    const auto exits = arm64::jit::semantic::sve_library_exits(index);
    total += exits;
    if (exits == 0U)
      continue;
    ++rows_taking_the_library;
    require(arm64::detail::decode_sve_non_streaming(
                arm64::jit::semantic::sve_library_exit_word(index), features) ==
            &arm64::detail::sme_encodings[index]);
  }
  require(rows_taking_the_library != 0U);
  require(total == arm64::jit::semantic::sve_library_exit_total());
  arm64::jit::semantic::reset_sve_library_exits();
  require(arm64::jit::semantic::sve_library_exit_total() == 0U);
  arm64::jit::semantic::count_sve_library_exits(false);
  std::printf("%zu lowered words stay inline from a clean state\n", checked);
}

/* A gather or scatter with one element that cannot be reached, or with every
 * element at one address: the lane that faults decides what a scatter has
 * written by then, and elements at one address are written in order. The
 * rest of the accesses are valid, so the JIT's own accesses run up to the
 * lane that sends the word to the library. */
void gather_lane_faults(const HostCapabilities &host, Tally &tally) {
  Random random{0x6a17'5e07U};
  const auto features = arm64::detail::implemented_sve_features(host);
  std::size_t checked = 0;
  for (std::size_t index = 0; index < arm64::detail::sme_encodings.size();
       ++index) {
    const auto &row = arm64::detail::sme_encodings[index];
    if (row.memory == 0U ||
        arm64::detail::sme_encoding_memory[row.memory - 1U].shape !=
            arm64::detail::SmeMemoryShape::gather ||
        (row.non_streaming_requirement & features) !=
            row.non_streaming_requirement)
      continue;
    const auto &memory = arm64::detail::sme_encoding_memory[row.memory - 1U];
    const auto *const operands = arm64::detail::sme_encoding_operands_of(row);
    Machine machine{host};
    for (std::size_t slot = 0; slot < 16U; ++slot) {
      const auto word = row.value | (static_cast<std::uint32_t>(random.next()) &
                                     ~row.mask);
      const auto *decoded = arm64::detail::decode_sve_non_streaming(word, features);
      if (decoded != &row)
        continue;
      std::vector<std::byte> data(mapped);
      for (std::size_t offset = 0; offset < mapped; offset += 8) {
        const auto value = random.next();
        std::memcpy(data.data() + offset, &value, 8);
      }
      auto initial = random_state(random, false);
      for (auto &predicate : initial.sve_p)
        predicate = arm64::sve_predicate_mask(0xffffU);
      initial.tbi_enabled = false;
      if (!place_clean_base(row, word, random, initial))
        continue;
      place_vectors(row, word, random, initial, true);
      const unsigned esize = arm64::detail::sme_memory_esize(memory, word) / 8U;
      const unsigned lanes = 16U / esize;
      const bool vector_base =
          operands[memory.base_operand].kind == arm64::detail::SmeOperandKind::z;
      const unsigned number = arm64::detail::sme_operand_number(
          operands[vector_base ? memory.base_operand : memory.index_operand],
          word);
      auto &vector = initial.vectors[number & 31U];
      const unsigned lane = static_cast<unsigned>(random.next() % lanes);
      std::uint64_t value = 0;
      if (slot % 2U == 0U) {
        /* One lane out of reach: far beyond the mapping. */
        value = vector_base ? code_base + mapped + (random.next() % page)
                            : (std::uint64_t{1} << 24) + (random.next() % page);
        std::memcpy(vector.bytes.data() + lane * esize, &value, esize);
      } else {
        /* Every lane the same: the first lane's. */
        std::memcpy(&value, vector.bytes.data(), esize);
        for (unsigned other = 1; other < lanes; ++other)
          std::memcpy(vector.bytes.data() + other * esize, &value, esize);
      }
      /* The first run leaves the TLB the accesses before the faulting lane
       * need, so the second makes them inline and leaves at the faulting
       * one. */
      compare(machine, slot, word, index, Variant::alone, initial, data, false,
              tally);
      compare(machine, slot, word, index, Variant::alone, initial, data, false,
              tally);
      ++checked;
    }
  }
  std::printf("%zu gathers and scatters with one lane out of reach or all at "
              "one address match the library\n",
              checked);
}

/* While another vCPU holds an exclusive reservation every store is tracked,
 * and a scatter, which the tracked variant leaves to the library, must still
 * leave what the library leaves. */
void scatters_with_tracked_stores(const HostCapabilities &host, Tally &tally) {
  Random random{0x6a17'5e09U};
  const auto features = arm64::detail::implemented_sve_features(host);
  arm64::CpuState holder;
  require(arm64::begin_jit_reservation(holder));
  std::size_t checked = 0;
  for (std::size_t index = 0; index < arm64::detail::sme_encodings.size();
       ++index) {
    const auto &row = arm64::detail::sme_encodings[index];
    if (row.memory == 0U ||
        arm64::detail::sme_encoding_memory[row.memory - 1U].shape !=
            arm64::detail::SmeMemoryShape::gather ||
        arm64::detail::sme_encoding_memory[row.memory - 1U].direction !=
            arm64::detail::SmeMemoryDirection::store ||
        (row.non_streaming_requirement & features) !=
            row.non_streaming_requirement)
      continue;
    Machine machine{host};
    for (std::size_t slot = 0; slot < 4U; ++slot) {
      const auto word = row.value | (static_cast<std::uint32_t>(random.next()) &
                                     ~row.mask);
      const auto *decoded = arm64::detail::decode_sve_non_streaming(word, features);
      if (decoded != &row)
        continue;
      std::vector<std::byte> data(mapped);
      for (std::size_t offset = 0; offset < mapped; offset += 8) {
        const auto value = random.next();
        std::memcpy(data.data() + offset, &value, 8);
      }
      auto initial = random_state(random, false);
      for (auto &predicate : initial.sve_p)
        predicate = arm64::sve_predicate_mask(0xffffU);
      initial.tbi_enabled = false;
      if (!place_clean_base(row, word, random, initial))
        continue;
      place_vectors(row, word, random, initial, true);
      compare(machine, slot, word, index, Variant::alone, initial, data, false,
              tally);
      compare(machine, slot, word, index, Variant::alone, initial, data, false,
              tally);
      ++checked;
    }
  }
  arm64::end_jit_reservation(holder);
  std::printf("%zu scatters run while another vCPU's reservation tracks "
              "stores\n",
              checked);
}

/* The scalar saturating counts (SQINC, SQDEC, UQINC, UQDEC of Xdn or Wdn)
 * at the values where they saturate: random registers are never near. */
void saturating_counts_at_their_limits(const HostCapabilities &host,
                                       Tally &tally) {
  Random random{0x6a17'5e0bU};
  const auto features = arm64::detail::implemented_sve_features(host);
  const std::array<std::uint64_t, 8> edges{
      0U,           0x7fff'ffffU, 0x8000'0000U,
      0xffff'ffffU, 0x1'0000'0000U, 0x7fff'ffff'ffff'ffffULL,
      0x8000'0000'0000'0000ULL, 0xffff'ffff'ffff'ffffULL};
  std::size_t checked = 0;
  for (std::size_t index = 0; index < arm64::detail::sme_encodings.size();
       ++index) {
    const auto &row = arm64::detail::sme_encodings[index];
    if ((row.value & 0xff20'f000U) != 0x0420'f000U ||
        (row.mask & 0xff20'f000U) != 0xff20'f000U ||
        (row.non_streaming_requirement & features) !=
            row.non_streaming_requirement)
      continue;
    for (std::size_t sample = 0; sample < 6U; ++sample) {
      const auto word = row.value | (static_cast<std::uint32_t>(random.next()) &
                                     ~row.mask);
      if (arm64::detail::decode_sve_non_streaming(word, features) != &row ||
          (word & 31U) == 31U)
        continue;
      /* A machine per word: it translates the program once, at slot 0. */
      Machine machine{host};
      for (const auto edge : edges)
        for (const std::uint64_t delta :
             {0ULL, 1ULL, 2ULL, 7ULL, 16ULL, 33ULL, 255ULL, 256ULL,
              static_cast<std::uint64_t>(-1), static_cast<std::uint64_t>(-2),
              static_cast<std::uint64_t>(-16), static_cast<std::uint64_t>(-256)}) {
          std::vector<std::byte> data(mapped);
          auto initial = random_state(random, false);
          initial.registers[word & 31U] = edge + delta;
          compare(machine, 0, word, index, Variant::alone, initial, data, false,
                  tally);
          ++checked;
        }
    }
  }
  std::printf("%zu saturating counts at their limits match the library\n",
              checked);
}

/* A predicate that PTRUE made all-active stops being known so as soon as
 * anything writes it: each writer below makes P0 partial for some state,
 * and the consumer after it, which the translation lowered for P0 all-active
 * only while it was known, must then leave the block for the library. */
void writers_forget_fullness(const HostCapabilities &host) {
  constexpr std::array<std::uint32_t, 8> writers{
      0x2518'e400U, /* pfalse p0.b */
      0x25a1'1c00U, /* whilelo p0.s, x0, x1 */
      0x2522'1fe0U, /* whilelo p0.b, xzr, x2 */
      0x2598'e060U, /* ptrue p0.s, vl3 */
      0x2502'4020U, /* and p0.b, p0/z, p1.b, p2.b */
      0x2519'f000U, /* rdffr p0.b */
      0x2581'4420U, /* orr p0.b, p1/z, p1.b, p1.b */
      0x2518'e3e0U, /* ptrue p0.b, all: stays known */
  };
  constexpr std::array<std::uint32_t, 11> consumers{
      0x65a2'0020U, /* fmla z0.s, p0/m, z1.s, z2.s */
      0xa540'a080U, /* ld1w {z0.s}, p0/z, [x4] */
      0xe540'e0a1U, /* st1w {z1.s}, p0, [x5] */
      0x0400'0020U, /* add z0.b, p0/m, z0.b, z1.b */
      0x0490'0020U, /* mul z0.s, p0/m, z0.s, z1.s */
      0xa420'a080U, /* ld1b {z0.h}, p0/z, [x4] */
      0xe460'e0a1U, /* st1b {z1.d}, p0, [x5] */
      0xa520'e080U, /* ld2w {z0.s, z1.s}, p0/z, [x4] */
      0x65c0'8083U, /* fadd z3.d, p0/m, z3.d, z4.d */
      0x0447'8060U, /* uqshl z0.s, p0/m, z0.s, #3 */
      0x8540'c080U, /* ld1rw {z0.s}, p0/z, [x4] */
  };
  Random random{0x6a17'5e02U};
  const auto features = arm64::detail::implemented_sve_features(host);
  std::size_t checked = 0;
  for (const auto writer : writers)
    for (const auto consumer : consumers) {
      /* One machine and one program per pair: the blocks it translates are
       * reused by the states below. */
      Machine machine{host};
      const std::array<std::uint32_t, 4> program{ptrue_b, writer, consumer,
                                                 svc};
      constexpr std::size_t offset = 0;
      for (unsigned sample = 0; sample < 12U; ++sample) {
        std::vector<std::byte> data(mapped);
        for (std::size_t at = 0; at < mapped; at += 8) {
          const auto value = random.next();
          std::memcpy(data.data() + at, &value, 8);
        }
        auto initial = random_state(random, false);
        for (unsigned n = 1; n < 16U; ++n)
          if (random.next() % 2U == 0U)
            initial.sve_p[n] = arm64::sve_predicate_mask(0xffffU);
        initial.registers[0] = random.next() % 40U;
        initial.registers[1] = random.next() % 40U;
        initial.registers[2] = random.next() % 40U;
        initial.registers[4] = code_base + page + 64U + random.next() % (page - 256U);
        initial.registers[5] = code_base + page + 64U + random.next() % (page - 256U);
        std::vector<std::byte> before = data;
        std::memcpy(before.data() + offset, program.data(), sizeof program);
        machine.put(0, before);
        machine.cpu = initial;
        std::size_t retired = 0;
        const auto event = machine.run(code_base + offset, retired);
        std::vector<std::byte> after(mapped);
        machine.get(0, after);

        auto expected = initial;
        CopiedMemory reference{before};
        for (const auto word : {ptrue_b, writer, consumer}) {
          const auto *row = arm64::detail::decode_sve_non_streaming(word, features);
          require(row != nullptr);
          const auto result = arm64::detail::execute_sve(expected, word, *row, reference);
          require(result.event == arm64::detail::SveEvent::retired);
        }
        bool same = event == Event::system_call && retired == 4U &&
                    machine.cpu.registers == expected.registers &&
                    machine.cpu.sve_p == expected.sve_p &&
                    machine.cpu.sve_ffr == expected.sve_ffr &&
                    machine.cpu.fpsr == expected.fpsr &&
                    machine.cpu.nzcv == expected.nzcv && after == before;
        for (unsigned n = 0; n < 32U && same; ++n)
          same = machine.cpu.vectors[n].bytes == expected.vectors[n].bytes;
        if (!same) {
          std::fprintf(stderr,
                       "writer %08x then consumer %08x: the JIT leaves another "
                       "state than the library (event %d, retired %zu)\n",
                       writer, consumer, static_cast<int>(event), retired);
          for (unsigned n = 0; n < 32U; ++n)
            if (machine.cpu.vectors[n].bytes != expected.vectors[n].bytes) {
              std::fprintf(stderr, "  Z%u differs\n", n);
              print_bytes("jit    ", machine.cpu.vectors[n].bytes);
              print_bytes("library", expected.vectors[n].bytes);
            }
          for (unsigned n = 0; n < 16U; ++n)
            if (machine.cpu.sve_p[n] != expected.sve_p[n])
              std::fprintf(stderr, "  P%u differs\n", n);
          if (machine.cpu.registers != expected.registers)
            std::fprintf(stderr, "  X differs\n");
          if (after != before)
            std::fprintf(stderr, "  memory differs\n");
          require(false);
        }
        ++checked;
      }
    }
  std::printf("%zu writer/consumer programs match the library\n", checked);
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
  writers_forget_fullness(sve_host());
  forms_match_the_library(sve_host(), tally);
  lowered_words_stay_inline(sve_host(), tally);
  gather_lane_faults(sve_host(), tally);
  scatters_with_tracked_stores(sve_host(), tally);
  saturating_counts_at_their_limits(sve_host(), tally);
  require(tally.rows > 400U && tally.runs > tally.rows && tally.faults > 0U &&
          tally.traps > 0U);
  std::printf("%zu runs over %zu rows (%zu inline) match the library, %zu of "
              "them faulting and %zu trapping\n",
              tally.runs, tally.rows, tally.inline_rows, tally.faults,
              tally.traps);
  return 0;
}
