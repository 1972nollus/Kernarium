// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The JIT's streaming memory forms at run time (S4 milestone M3). They run
 * natively in SM=1 translations, so every test needs a host whose profile
 * implements SME (an SME host with DARWIN_UML_GUEST_SME=1) and is skipped
 * elsewhere; arm64-jit-sme-codegen checks the translations on any host.
 *
 * Every memory form the JIT implements runs twice on the same registers,
 * predicates and data: inside one guest page, where the guest's own word
 * runs on the host address, and across a page boundary, where the helper
 * runs it on the register file. The two results must be identical, which
 * holds the helper to the hardware. Faults follow the M4: a predicated
 * access reports its first faulting active byte rounded down to 4 KiB, an
 * unpredicated one that byte, an access with no active element on the
 * unmapped page does not fault, and a faulting store writes nothing.
 */
#include "arm64_jit_sme_test_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/jit/jit_sme_support.hpp"
#include "cpu/arm64/sme_profile.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace uml::emulator::tests;
using namespace uml::emulator::tests::jit_sme;

namespace {

namespace detail = arm64::detail;
using Shape = detail::SmeMemoryShape;

constexpr std::uint32_t ptrue_p1_s = 0x2598'e3e1U;
constexpr std::uint32_t ptrue_p2_s = 0x2598'e3e2U;
constexpr std::uint32_t whilelo_p3_s_x6 = 0x25a6'1fe3U; // whilelo p3.s, xzr, x6
constexpr std::uint32_t whilelo_p1_s_x6 = 0x25a6'1fe1U; // whilelo p1.s, xzr, x6
constexpr std::uint32_t bic_p1_p2_p3 = 0x2503'4851U; // bic p1.b, p2/z, p2.b, p3.b
constexpr std::uint32_t pfalse_p1 = 0x2518'e401U;
constexpr std::uint32_t ld1w_z4_x2 = 0xa540'a444U;   // ld1w {z4.s}, p1/z, [x2]
constexpr std::uint32_t st1w_z4_x2 = 0xe540'e444U;   // st1w {z4.s}, p1, [x2]
constexpr std::uint32_t ldr_z4_x2 = 0x8580'4044U;    // ldr z4, [x2]
constexpr std::uint32_t str_z4_x2 = 0xe580'4044U;    // str z4, [x2]
constexpr std::uint32_t ld1rqw_z4_x2 = 0xa500'2444U; // ld1rqw {z4.s}, p1/z, [x2]
constexpr std::uint32_t ld1rw_z4_x2 = 0x8540'c444U;  // ld1rw {z4.s}, p1/z, [x2]
/* ld1w and st1w {za1h.s[w13, 1]}, p1, [x2] */
constexpr std::uint32_t ld1w_za_x2 = 0xe09f'2445U;
constexpr std::uint32_t st1w_za_x2 = 0xe0bf'2445U;
constexpr std::uint32_t ld2d_sp = 0xa5a0'e7e4U; // ld2d {z4.d, z5.d}, p1/z, [sp]
constexpr std::uint32_t st2d_sp = 0xe5b0'e7e4U; // st2d {z4.d, z5.d}, p1, [sp]

/* Registers the generated words name. */
constexpr unsigned base_register = 2;
constexpr unsigned index_register = 3;
constexpr unsigned slice_register = 13;
constexpr unsigned transfer_z = 4;
constexpr unsigned transfer_p = 5;
constexpr unsigned governing_p = 1;
constexpr std::uint64_t index_value = 5;
constexpr std::uint64_t slice_value = 7;
/* Bytes compared on each side of an access. */
constexpr std::size_t margin = 512;

unsigned long next_generation = 9600;

[[nodiscard]] std::uint64_t next_base() noexcept {
  return 0x400'0000U + (next_generation - 9600U) * 0x10'0000U;
}

/* `word` with an operand naming register `number`; its constant segments
 * must agree. */
[[nodiscard]] std::uint32_t with_operand(std::uint32_t word,
                                         const detail::SmeOperand &operand,
                                         unsigned number) {
  unsigned shift = 0;
  for (unsigned index = operand.segment_count; index-- > 0U;) {
    const auto &segment = operand.segments[index];
    const unsigned value = (number >> shift) & ((1U << segment.width) - 1U);
    if (segment.field) {
      const auto mask = ((1U << segment.width) - 1U) << segment.low;
      word = (word & ~mask) | (value << segment.low);
    } else {
      require(value == segment.constant);
    }
    shift += segment.width;
  }
  return word;
}

[[nodiscard]] std::uint32_t with_immediate(std::uint32_t word,
                                           const detail::SmeMemory &memory,
                                           std::int64_t value) {
  unsigned shift = 0;
  for (unsigned index = memory.immediate_segment_count; index-- > 0U;) {
    const auto &segment = memory.immediate[index];
    const auto bits = static_cast<unsigned>(
        (static_cast<std::uint64_t>(value) >> shift) &
        ((1U << segment.width) - 1U));
    const auto mask = ((1U << segment.width) - 1U) << segment.low;
    word = (word & ~mask) | (bits << segment.low);
    shift += segment.width;
  }
  return word;
}

struct Form final {
  std::uint32_t word;
  const detail::SmeEncoding *row;
  const detail::SmeMemory *memory;
};

/* One word for every memory form the JIT implements but prefetches, with
 * the registers above, a nonzero immediate and, for a ZA slice, tile and
 * offset bits 0101, horizontal and vertical. */
[[nodiscard]] std::vector<Form> forms() {
  std::vector<Form> result;
  std::vector<std::uint16_t> seen;
  for (const auto &row : detail::sme_encodings) {
    if (row.memory == 0U || !arm64::jit::implements_sme_row(row) ||
        arm64::jit::implements_sme_form(row.form))
      continue;
    const auto &memory = *detail::sme_encoding_memory_of(row);
    if (memory.shape == Shape::prefetch ||
        std::ranges::find(seen, row.form) != seen.end())
      continue;
    seen.push_back(row.form);
    auto word = row.value;
    const auto *operands = detail::sme_encoding_operands_of(row);
    for (unsigned index = 0; index < row.operand_count; ++index) {
      const auto &operand = operands[index];
      unsigned number = 0;
      switch (operand.kind) {
      case detail::SmeOperandKind::gpr_or_sp:
        number = base_register;
        break;
      case detail::SmeOperandKind::gpr:
        number = operand.segment_count == 2U ? slice_register : index_register;
        break;
      case detail::SmeOperandKind::z:
        number = transfer_z;
        break;
      case detail::SmeOperandKind::p:
        number = memory.shape == Shape::fill_p ? transfer_p : governing_p;
        break;
      default:
        continue;
      }
      word = with_operand(word, operand, number);
    }
    std::int64_t immediate = 0;
    switch (memory.shape) {
    case Shape::replicate:
      immediate = 3;
      break;
    case Shape::replicate_quad:
    case Shape::contiguous:
    case Shape::structure:
      immediate = -1;
      break;
    case Shape::fill_z:
    case Shape::fill_p:
      immediate = 2;
      break;
    default:
      break;
    }
    word = with_immediate(word, memory, immediate);
    if (memory.shape == Shape::za_slice)
      word = (word & ~15U) | 5U;
    const auto *found = detail::find_sme_encoding(word);
    require(found != nullptr && found->form == row.form);
    result.push_back({word, found, &memory});
    if (memory.shape == Shape::za_slice) {
      const auto vertical = word | 0x8000U;
      require(detail::find_sme_encoding(vertical)->form == row.form);
      result.push_back({vertical, found, &memory});
    }
  }
  return result;
}

struct Outcome final {
  Event event;
  std::size_t retired;
  std::vector<std::byte> file;
  std::vector<std::byte> window;
};

/* Runs `word` once with its access at data(offset) of a fresh machine,
 * with the data around it from `seed`; `tagged` sets a tag in the base's
 * top byte and enables TBI. */
[[nodiscard]] Outcome run_at(const Form &form, std::size_t offset,
                             unsigned seed, bool tagged = false) {
  const auto mode = form.memory->shape == Shape::za_slice
                        ? static_cast<std::uint8_t>(arm64::svcr_mask)
                        : arm64::svcr_sm;
  Machine machine{next_generation, next_base(), mode, true, 6, 6};
  ++next_generation;
  const auto svl = machine.svl();
  const auto span = arm64::jit::sme_memory_span(*form.memory, form.word, svl);
  const auto relative =
      arm64::jit::sme_memory_offset(*form.memory, form.word, svl, index_value);
  const auto address = machine.data(offset);
  machine.code(0, {form.word, system_call});
  machine.cpu.registers[base_register] = address - relative;
  if (tagged) {
    machine.cpu.tbi_enabled = true;
    machine.cpu.registers[base_register] |= 0x5a00'0000'0000'0000ULL;
  }
  machine.cpu.registers[index_register] = index_value;
  machine.cpu.registers[slice_register] = slice_value;
  const auto window = bytes_of(span + 2U * margin, seed);
  machine.put(address - margin, window);
  std::size_t retired = 0;
  Outcome outcome{};
  outcome.event = machine.run(retired);
  outcome.retired = retired;
  outcome.file = machine.file();
  outcome.window = machine.get(address - margin, span + 2U * margin);
  machine.require_preserved();
  return outcome;
}

/* Every form, in one page, across the first data page's end (an element
 * straddles it unless the elements are bytes) and with a tagged base,
 * gives the same registers and memory. The last two take the helper. */
void slow_path_matches_the_hardware() {
  std::size_t count = 0;
  for (const auto &form : forms()) {
    const auto svl = std::size_t{uml::cpu::host_capabilities().sme_svl_bytes};
    const auto span = arm64::jit::sme_memory_span(*form.memory, form.word, svl);
    for (const std::size_t skew : {std::size_t{0}, std::size_t{3}}) {
      const auto fast = run_at(form, 0x800U - skew, 40U + skew);
      for (const bool tagged : {false, true}) {
        const auto slow =
            tagged ? run_at(form, 0x800U - skew, 40U + skew, true)
                   : run_at(form, page - span / 2U - skew, 40U + skew);
        if (fast.event != Event::system_call ||
            slow.event != Event::system_call || fast.retired != 2U ||
            slow.retired != 2U || fast.file != slow.file ||
            fast.window != slow.window) {
          std::fprintf(stderr, "%08x skew %zu tagged %d: events %d %d, "
                       "retired %zu %zu, file %s, memory %s\n", form.word,
                       skew, tagged, static_cast<int>(fast.event),
                       static_cast<int>(slow.event), fast.retired,
                       slow.retired,
                       fast.file == slow.file ? "same" : "differs",
                       fast.window == slow.window ? "same" : "differs");
          require(false);
        }
      }
    }
    ++count;
  }
  std::fprintf(stderr, "%zu memory words agree in one page and across "
               "two\n", count);
  require(count > 100U);
}

/* SP as the base, aligned, in one page and across two. */
void stack_pointer_bases() {
  for (const auto word : {ld2d_sp, st2d_sp}) {
    std::vector<std::byte> files[2];
    std::vector<std::byte> windows[2];
    for (unsigned crossing = 0; crossing < 2U; ++crossing) {
      Machine machine{next_generation, next_base(), arm64::svcr_sm, true, 6,
                      6};
      ++next_generation;
      const std::size_t svl = machine.svl();
      const auto address =
          machine.data(crossing != 0U ? page - svl : std::size_t{0x800});
      machine.code(0, {ptrue_p1_s, word, system_call});
      machine.cpu.sp = address;
      machine.put(address - margin, bytes_of(2U * svl + 2U * margin, 9));
      std::size_t retired = 0;
      require(machine.run(retired) == Event::system_call);
      require(retired == 3U);
      files[crossing] = machine.file();
      windows[crossing] = machine.get(address - margin, 2U * svl + 2U * margin);
      machine.require_preserved();
    }
    require(files[0] == files[1]);
    require(windows[0] == windows[1]);
  }
}

struct FaultCase final {
  const char *name;
  std::vector<std::uint32_t> prologue;
  std::uint32_t word;
  bool za;
  bool load;
  /* The access's start, relative to the first unmapped page. */
  std::int64_t start;
  /* x6, for WHILELO. */
  std::uint64_t count;
  /* The expected fault address relative to that page, or -1 for none. */
  std::int64_t fault;
};

/* The cases' expectations are for any SVL: `start` and `count` scale
 * with it below. */
void faults_follow_the_m4() {
  const std::size_t svl = std::size_t{uml::cpu::host_capabilities().sme_svl_bytes};
  const auto half = static_cast<std::int64_t>(svl / 2U);
  const auto words = svl / 4U;
  const std::vector<FaultCase> cases{
      /* Every element active: the first byte on the unmapped page. */
      {"ld1w all", {ptrue_p1_s}, ld1w_z4_x2, false, true, -half, 0, 0},
      {"st1w all", {ptrue_p1_s}, st1w_z4_x2, false, false, -half, 0, 0},
      /* Active from three words past the boundary: 12 bytes in, rounded
       * down to 4 KiB. */
      {"ld1w late", {ptrue_p2_s, whilelo_p3_s_x6, bic_p1_p2_p3}, ld1w_z4_x2,
       false, true, -half, words / 2U + 3U, 0},
      {"st1w late", {ptrue_p2_s, whilelo_p3_s_x6, bic_p1_p2_p3}, st1w_z4_x2,
       false, false, -half, words / 2U + 3U, 0},
      /* Only the mapped half active: no fault. */
      {"ld1w mapped", {whilelo_p1_s_x6}, ld1w_z4_x2, false, true, -half,
       words / 2U, -1},
      {"st1w mapped", {whilelo_p1_s_x6}, st1w_z4_x2, false, false, -half,
       words / 2U, -1},
      /* Wholly unmapped: predicated at 4 KiB, unpredicated exact. */
      {"ld1w unmapped", {ptrue_p1_s}, ld1w_z4_x2, false, true, 0x10, 0, 0},
      {"ldr unmapped", {}, ldr_z4_x2, false, true, 0x10, 0, 0x10},
      {"str unmapped", {}, str_z4_x2, false, false, 0x10, 0, 0x10},
      {"ldr crossing", {}, ldr_z4_x2, false, true, -half, 0, 0},
      /* No active element: nothing accessed. */
      {"ld1w none", {pfalse_p1}, ld1w_z4_x2, false, true, 0x10, 0, -1},
      {"st1w none", {pfalse_p1}, st1w_z4_x2, false, false, 0x10, 0, -1},
      {"ld1rw none", {pfalse_p1}, ld1rw_z4_x2, false, true, 0x10, 0, -1},
      {"ld1rw all", {ptrue_p1_s}, ld1rw_z4_x2, false, true, 0x10, 0, 0},
      {"ld1rqw late", {ptrue_p2_s, whilelo_p3_s_x6, bic_p1_p2_p3},
       ld1rqw_z4_x2, false, true, -8, 2, 0},
      {"ld1rqw mapped", {whilelo_p1_s_x6}, ld1rqw_z4_x2, false, true, -8, 2,
       -1},
      {"za load late", {ptrue_p2_s, whilelo_p3_s_x6, bic_p1_p2_p3},
       ld1w_za_x2, true, true, -half, words / 2U + 3U, 0},
      {"za store all", {ptrue_p1_s}, st1w_za_x2, true, false, -half, 0, 0},
      {"za store mapped", {whilelo_p1_s_x6}, st1w_za_x2, true, false, -half,
       words / 2U, -1},
  };
  for (const auto &test : cases) {
    const auto mode = test.za ? static_cast<std::uint8_t>(arm64::svcr_mask)
                              : arm64::svcr_sm;
    /* The code page and the first data page are mapped. */
    Machine machine{next_generation, next_base(), mode, true, 4, 2};
    ++next_generation;
    std::vector<std::uint32_t> program{test.prologue};
    program.push_back(test.word);
    program.push_back(system_call);
    for (std::size_t index = 0; index < program.size(); ++index)
      machine.code(index * 4U, {program[index]});
    const auto boundary = machine.data(page);
    const auto address =
        boundary + static_cast<std::uint64_t>(test.start);
    machine.cpu.registers[base_register] = address;
    machine.cpu.registers[6] = test.count;
    machine.cpu.registers[slice_register] = slice_value;
    const std::size_t before = margin;
    machine.put(boundary - before, bytes_of(before, 21));
    const auto file = machine.initial_file();
    std::size_t retired = 0;
    const auto event = machine.run(retired);
    const auto prologue = test.prologue.size();
    if (test.fault < 0) {
      if (event != Event::system_call || retired != prologue + 2U) {
        std::fprintf(stderr, "%s: event %d, retired %zu\n", test.name,
                     static_cast<int>(event), retired);
        require(false);
      }
      continue;
    }
    const auto expected = boundary + static_cast<std::uint64_t>(test.fault);
    if (event != Event::memory_fault || retired != prologue ||
        machine.executor().fault_address() != expected ||
        machine.executor().fault_alignment() ||
        machine.executor().fault_access() !=
            (test.load ? uml::memory::Access::read
                       : uml::memory::Access::write)) {
      std::fprintf(stderr, "%s: event %d, retired %zu, pc %#llx, fault "
                   "%#llx, expected %#llx\n", test.name,
                   static_cast<int>(event), retired,
                   static_cast<unsigned long long>(machine.cpu.pc),
                   static_cast<unsigned long long>(
                       machine.executor().fault_address()),
                   static_cast<unsigned long long>(expected));
      require(false);
    }
    /* A faulting store wrote nothing, and a faulting load changed no
     * register (the prologue's predicates aside). */
    require(machine.get(boundary - before, before) == bytes_of(before, 21));
    const auto after = machine.file();
    const auto z = arm64::sme_z_offset(machine.svl(), transfer_z);
    require(std::equal(after.begin() + static_cast<std::ptrdiff_t>(z),
                       after.begin() +
                           static_cast<std::ptrdiff_t>(z + machine.svl()),
                       file.begin() + static_cast<std::ptrdiff_t>(z)));
    /* WHILELO sets the flags. */
    machine.require_preserved({}, true);
  }
  std::fprintf(stderr, "%zu fault cases follow the M4\n", cases.size());
}

/* An SP base that is not 16-byte aligned faults on alignment, at SP,
 * before any access. */
void stack_pointer_alignment() {
  Machine machine{next_generation, next_base(), arm64::svcr_sm, true, 6, 6};
  ++next_generation;
  machine.code(0, {ptrue_p1_s, ld2d_sp, system_call});
  machine.cpu.sp = machine.data(0x808);
  std::size_t retired = 0;
  require(machine.run(retired) == Event::memory_fault);
  require(retired == 1U);
  require(machine.executor().fault_alignment());
  require(machine.executor().fault_address() == machine.data(0x808));
  require(machine.executor().fault_access() == uml::memory::Access::read);
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  if (uml::cpu::arm64::jit::sme_support == uml::cpu::arm64::SmeSupport::none ||
      !jit_available() ||
      !uml::cpu::arm64::implements_sme(uml::cpu::host_capabilities())) {
    std::printf("skipped: SM=1 code needs a host profile with SME\n");
    return 0;
  }
  slow_path_matches_the_hardware();
  stack_pointer_bases();
  faults_follow_the_m4();
  stack_pointer_alignment();
  return 0;
}
