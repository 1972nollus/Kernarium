// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The JIT's non-streaming SVE control core, the forms an SM=0 translation
 * lowers inline however the rest of the profile runs (jit::sve_support =
 * complete sends every other row to the semantic library, which
 * arm64-jit-sve-forms checks against it): PTRUE, PTRUES, PFALSE and PTEST, the predicate logical operations, CNTP,
 * INCP and DECP, WHILE* and its fusion with B.cond, CNT*, INC*, DEC*, RDVL,
 * ADDVL and ADDPL, SETFFR, RDFFR and WRFFR, and MOVPRFX, DUP, DUPM, FDUP,
 * CPY, FCPY, SEL and the unpredicated logical forms, lowered onto AdvSIMD
 * and general registers at VL 16.
 *
 * No host implements SVE, so each test injects the SVE profile into the
 * capabilities the executor translates for and drives CpuState directly:
 * P and FFR in their mask form, Z as `vectors`. A reference interpreter
 * follows the A64 pseudocode element by element, independently of the
 * lowering, and runs random programs beside the JIT. The code checks
 * compile every row of the encoding table: at SM=0 every implemented row
 * translates except a reserved encoding, and no translation holds an SVE
 * word; SM=1 translations, and translations without an SVE word, are the
 * same with the profile and without it.
 */
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/detail/sve_semantics.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
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
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

namespace {

namespace arm64 = uml::cpu::arm64;
namespace forms = arm64::jit::sve_forms;
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

constexpr std::uint64_t code_base = 0x93'0000;
constexpr std::uint32_t svc = 0xd400'0001U;

/* Hosts. */

void clear(HostCapabilities &host, std::initializer_list<ArmFeature> features) {
  for (const auto feature : features)
    host.supported.reset(static_cast<std::size_t>(feature));
}

/* The host's profile without SME, with SVE and SVE2 unless `sve` is false;
 * the other SVE tokens stay off. */
[[nodiscard]] HostCapabilities sve_host(bool sve = true) {
  auto host = uml::cpu::host_capabilities();
  clear(host, {ArmFeature::sme, ArmFeature::sme2, ArmFeature::sme_f64f64,
               ArmFeature::sme_i16i64, ArmFeature::sve, ArmFeature::sve2,
               ArmFeature::sve_aes, ArmFeature::sve_pmull128,
               ArmFeature::sve_sha3});
  host.sme_svl_bytes = 0;
  host.sme_hardware = false;
  if (sve)
    for (const auto feature : {ArmFeature::sve, ArmFeature::sve2})
      host.supported.set(static_cast<std::size_t>(feature));
  return host;
}

/* With an injected SME2 profile at SVL 64 too, which never claims the
 * hardware. */
[[nodiscard]] HostCapabilities sve_sme_host(bool sve = true) {
  auto host = sve_host(sve);
  for (const auto feature : {ArmFeature::sme, ArmFeature::sme2,
                             ArmFeature::sme_f64f64, ArmFeature::sme_i16i64})
    host.supported.set(static_cast<std::size_t>(feature));
  host.sme_svl_bytes = 64;
  return host;
}

/* Encoders for the directed programs. */

[[nodiscard]] constexpr std::uint32_t ptrue(unsigned d, unsigned size,
                                            unsigned pattern = 31U,
                                            bool flags = false) {
  return 0x2518'e000U | (size << 22) | (flags ? 1U << 16 : 0U) |
         (pattern << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t whilelo(unsigned d, unsigned size,
                                              unsigned n, unsigned m,
                                              bool wide = true) {
  return 0x2520'0c00U | (size << 22) | (m << 16) | (wide ? 1U << 12 : 0U) |
         (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t incw(unsigned d) {
  return 0x04b0'e3e0U | d; // INCW Xd (ALL, MUL #1)
}
[[nodiscard]] constexpr std::uint32_t b_cond(unsigned condition, int words) {
  return 0x5400'0000U |
         ((static_cast<std::uint32_t>(words) & 0x7'ffffU) << 5) | condition;
}
[[nodiscard]] constexpr std::uint32_t branch(int words) {
  return 0x1400'0000U | (static_cast<std::uint32_t>(words) & 0x03ff'ffffU);
}
[[nodiscard]] constexpr std::uint32_t add_immediate(unsigned d, unsigned n,
                                                    unsigned immediate) {
  return 0x9100'0000U | (immediate << 10) | (n << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t dup_indexed(unsigned d, unsigned n,
                                                  unsigned esize,
                                                  unsigned index) {
  const unsigned log = static_cast<unsigned>(std::countr_zero(esize));
  const unsigned immediate = (index << (log + 1U)) | (1U << log);
  return 0x0520'2000U | ((immediate >> 5) << 22) | ((immediate & 31U) << 16) |
         (n << 5) | d;
}

/* The architectural state a program changes. */
struct State final {
  std::array<std::uint64_t, 31> x{};
  std::uint64_t sp{};
  std::uint32_t nzcv{};
  std::array<Bytes, 32> z{};
  std::array<std::uint16_t, 16> p{};
  std::uint16_t ffr{};
  std::uint64_t pc{};

  friend bool operator==(const State &, const State &) = default;
};

[[nodiscard]] constexpr bool canonical(const arm64::SvePredicate &mask) {
  return std::ranges::all_of(
      mask, [](std::uint8_t byte) { return byte == 0U || byte == 0xffU; });
}

void put_state(arm64::CpuState &cpu, const State &state) {
  cpu.registers = state.x;
  cpu.sp = state.sp;
  cpu.nzcv = state.nzcv;
  for (unsigned n = 0; n < 32U; ++n)
    std::memcpy(cpu.vectors[n].bytes.data(), state.z[n].data(), 16);
  for (unsigned n = 0; n < 16U; ++n)
    cpu.sve_p[n] = arm64::sve_predicate_mask(state.p[n]);
  cpu.sve_ffr = arm64::sve_predicate_mask(state.ffr);
  cpu.pc = state.pc;
}

[[nodiscard]] State get_state(arm64::CpuState &cpu) {
  arm64::restore_native_fp_environment(cpu);
  cpu.access_fp_state();
  State state;
  state.x = cpu.registers;
  state.sp = cpu.sp;
  state.nzcv = cpu.nzcv;
  for (unsigned n = 0; n < 32U; ++n)
    std::memcpy(state.z[n].data(), cpu.vectors[n].bytes.data(), 16);
  for (unsigned n = 0; n < 16U; ++n) {
    require(canonical(cpu.sve_p[n]));
    state.p[n] = arm64::sve_predicate_packed(cpu.sve_p[n]);
  }
  require(canonical(cpu.sve_ffr));
  state.ffr = arm64::sve_predicate_packed(cpu.sve_ffr);
  state.pc = cpu.pc;
  return state;
}

void print_state_difference(const State &got, const State &want) {
  for (unsigned n = 0; n < 31U; ++n)
    if (got.x[n] != want.x[n])
      std::fprintf(stderr, "  x%u %#llx, expected %#llx\n", n,
                   static_cast<unsigned long long>(got.x[n]),
                   static_cast<unsigned long long>(want.x[n]));
  if (got.sp != want.sp)
    std::fprintf(stderr, "  sp %#llx, expected %#llx\n",
                 static_cast<unsigned long long>(got.sp),
                 static_cast<unsigned long long>(want.sp));
  if (got.nzcv != want.nzcv)
    std::fprintf(stderr, "  nzcv %#x, expected %#x\n", got.nzcv, want.nzcv);
  for (unsigned n = 0; n < 32U; ++n)
    if (got.z[n] != want.z[n]) {
      std::fprintf(stderr, "  z%u", n);
      for (const auto byte : got.z[n])
        std::fprintf(stderr, " %02x", byte);
      std::fprintf(stderr, ", expected");
      for (const auto byte : want.z[n])
        std::fprintf(stderr, " %02x", byte);
      std::fprintf(stderr, "\n");
    }
  for (unsigned n = 0; n < 16U; ++n)
    if (got.p[n] != want.p[n])
      std::fprintf(stderr, "  p%u %#06x, expected %#06x\n", n, got.p[n],
                   want.p[n]);
  if (got.ffr != want.ffr)
    std::fprintf(stderr, "  ffr %#06x, expected %#06x\n", got.ffr, want.ffr);
  if (got.pc != want.pc)
    std::fprintf(stderr, "  pc %#llx, expected %#llx\n",
                 static_cast<unsigned long long>(got.pc),
                 static_cast<unsigned long long>(want.pc));
}

/* The reference: the A64 pseudocode at VL 16, element by element. */

[[nodiscard]] constexpr unsigned field(std::uint32_t word, unsigned low,
                                       unsigned width) {
  return (word >> low) & ((1U << width) - 1U);
}

[[nodiscard]] constexpr bool active(std::uint16_t predicate, unsigned esize,
                                    unsigned element) {
  return ((predicate >> (element * esize)) & 1U) != 0U;
}

[[nodiscard]] constexpr std::uint16_t all_true(unsigned esize) {
  std::uint16_t result = 0;
  for (unsigned element = 0; element < 16U / esize; ++element)
    result = static_cast<std::uint16_t>(result | (1U << (element * esize)));
  return result;
}

/* PredTest(): N = FirstActive, Z = NoneActive, C = !LastActive. */
[[nodiscard]] constexpr std::uint32_t pred_test(std::uint16_t mask,
                                                std::uint16_t result,
                                                unsigned esize) {
  bool seen = false;
  bool n = false;
  bool z = true;
  bool last = false;
  for (unsigned element = 0; element < 16U / esize; ++element) {
    if (!active(mask, esize, element))
      continue;
    const bool set = active(result, esize, element);
    if (!seen)
      n = set;
    seen = true;
    z = z && !set;
    last = set;
  }
  return (n ? 0x8000'0000U : 0U) | (z ? 0x4000'0000U : 0U) |
         (last ? 0U : 0x2000'0000U);
}

/* DecodePredCount(). */
[[nodiscard]] constexpr unsigned pattern_count(unsigned pattern,
                                               unsigned elements) {
  if (pattern == 0U) {
    unsigned power = 1;
    while (power * 2U <= elements)
      power *= 2U;
    return power;
  }
  if (pattern >= 1U && pattern <= 8U)
    return elements >= pattern ? pattern : 0U;
  if (pattern >= 9U && pattern <= 13U) {
    const unsigned count = 16U << (pattern - 9U);
    return elements >= count ? count : 0U;
  }
  if (pattern == 29U)
    return elements - elements % 4U;
  if (pattern == 30U)
    return elements - elements % 3U;
  if (pattern == 31U)
    return elements;
  return 0;
}

[[nodiscard]] constexpr std::uint16_t first_elements(unsigned esize,
                                                     unsigned count) {
  std::uint16_t result = 0;
  for (unsigned element = 0; element < count && element < 16U / esize;
       ++element)
    result = static_cast<std::uint16_t>(result | (1U << (element * esize)));
  return result;
}

/* VFPExpandImm(). */
[[nodiscard]] constexpr std::uint64_t vfp_expand(unsigned imm8,
                                                 unsigned bits) {
  const unsigned e = bits == 16U ? 5U : bits == 32U ? 8U : 11U;
  const unsigned f = bits - e - 1U;
  const std::uint64_t sign = imm8 >> 7;
  const std::uint64_t b = (imm8 >> 6) & 1U;
  std::uint64_t exponent = (b ^ 1U);
  for (unsigned index = 0; index < e - 3U; ++index)
    exponent = (exponent << 1) | b;
  exponent = (exponent << 2) | ((imm8 >> 4) & 3U);
  const std::uint64_t fraction = std::uint64_t{imm8 & 15U} << (f - 4U);
  return (sign << (bits - 1U)) | (exponent << f) | fraction;
}

static_assert(vfp_expand(0x70U, 32U) == 0x3f80'0000U);
static_assert(vfp_expand(0x70U, 16U) == 0x3c00U);
static_assert(vfp_expand(0x80U, 64U) == 0xc000'0000'0000'0000ULL);

[[nodiscard]] std::uint64_t element_of(const Bytes &bytes, unsigned esize,
                                       unsigned index) {
  std::uint64_t value = 0;
  std::memcpy(&value, bytes.data() + index * esize, esize);
  return value;
}

void set_element(Bytes &bytes, unsigned esize, unsigned index,
                 std::uint64_t value) {
  std::memcpy(bytes.data() + index * esize, &value, esize);
}

[[nodiscard]] Bytes splat(std::uint64_t value, unsigned esize) {
  Bytes bytes{};
  for (unsigned index = 0; index < 16U / esize; ++index)
    set_element(bytes, esize, index, value);
  return bytes;
}

[[nodiscard]] constexpr bool condition_holds(unsigned condition,
                                             std::uint32_t nzcv) {
  const bool n = (nzcv >> 31) & 1U;
  const bool z = (nzcv >> 30) & 1U;
  const bool c = (nzcv >> 29) & 1U;
  const bool v = (nzcv >> 28) & 1U;
  bool result = true;
  switch (condition >> 1) {
  case 0: result = z; break;
  case 1: result = c; break;
  case 2: result = n; break;
  case 3: result = v; break;
  case 4: result = c && !z; break;
  case 5: result = n == v; break;
  case 6: result = n == v && !z; break;
  default: result = true; break;
  }
  if ((condition & 1U) != 0U && condition != 15U)
    result = !result;
  return result;
}

class Reference final {
public:
  explicit Reference(State state) : s_{state} {}

  [[nodiscard]] const State &state() const noexcept { return s_; }
  [[nodiscard]] std::size_t retired() const noexcept { return retired_; }

  /* Runs from pc until SVC, which retires; false if it meets a word it
   * does not model. */
  [[nodiscard]] bool run(std::span<const std::uint32_t> program) {
    for (std::size_t steps = 0; steps < 4096U; ++steps) {
      const auto index = (s_.pc - code_base) / 4U;
      if (index >= program.size())
        return false;
      const auto word = program[index];
      ++retired_;
      if (word == svc) {
        s_.pc += 4U;
        return true;
      }
      if ((word & 0xff00'0010U) == 0x5400'0000U) {
        s_.pc = condition_holds(word & 15U, s_.nzcv)
                    ? s_.pc + static_cast<std::uint64_t>(
                                  arm64::detail::sign_extend(
                                      field(word, 5, 19), 19) *
                                  4)
                    : s_.pc + 4U;
        continue;
      }
      if ((word & 0xfc00'0000U) == 0x1400'0000U) {
        s_.pc += static_cast<std::uint64_t>(
            arm64::detail::sign_extend(field(word, 0, 26), 26) * 4);
        continue;
      }
      if ((word & 0xffc0'0000U) == 0x9100'0000U) {
        set_xsp(field(word, 0, 5),
                xsp(field(word, 5, 5)) + field(word, 10, 12));
        s_.pc += 4U;
        continue;
      }
      if (!sve(word))
        return false;
      s_.pc += 4U;
    }
    return false;
  }

private:
  [[nodiscard]] std::uint64_t x(unsigned n) const {
    return n == 31U ? 0U : s_.x[n];
  }
  void set_x(unsigned n, std::uint64_t value) {
    if (n != 31U)
      s_.x[n] = value;
  }
  [[nodiscard]] std::uint64_t xsp(unsigned n) const {
    return n == 31U ? s_.sp : s_.x[n];
  }
  void set_xsp(unsigned n, std::uint64_t value) {
    (n == 31U ? s_.sp : s_.x[n]) = value;
  }

  [[nodiscard]] bool sve(std::uint32_t word);
  void whiles(std::uint32_t word, std::uint16_t form);

  State s_;
  std::size_t retired_{};
};

void Reference::whiles(std::uint32_t word, std::uint16_t form) {
  const unsigned esize = 1U << field(word, 22, 2);
  const unsigned elements = 16U / esize;
  std::uint16_t result = 0;
  if (form == forms::whilewr || form == forms::whilerw) {
    const bool rw = form == forms::whilerw;
    __int128 difference = static_cast<__int128>(x(field(word, 16, 5))) -
                          static_cast<__int128>(x(field(word, 5, 5)));
    if (rw && difference < 0)
      difference = -difference;
    __int128 quotient = difference / esize;
    if (difference % esize != 0 && difference < 0)
      --quotient;
    for (unsigned element = 0; element < elements; ++element)
      if (rw ? (quotient == 0 || element < quotient)
             : (quotient <= 0 || element < quotient))
        result = static_cast<std::uint16_t>(result | (1U << (element * esize)));
  } else {
    const bool wide = field(word, 12, 1) != 0U;
    const bool is_unsigned = field(word, 11, 1) != 0U;
    const bool increasing = field(word, 10, 1) != 0U;
    const bool eq = field(word, 4, 1) != 0U;
    const std::uint64_t mask = wide ? ~std::uint64_t{0} : 0xffff'ffffU;
    std::uint64_t operand1 = x(field(word, 5, 5)) & mask;
    const std::uint64_t operand2 = x(field(word, 16, 5)) & mask;
    const auto value = [&](std::uint64_t operand) -> __int128 {
      if (is_unsigned)
        return operand;
      return wide ? static_cast<__int128>(static_cast<std::int64_t>(operand))
                  : static_cast<__int128>(static_cast<std::int32_t>(
                        static_cast<std::uint32_t>(operand)));
    };
    bool last = true;
    for (unsigned step = 0; step < elements; ++step) {
      const unsigned element = increasing ? step : elements - 1U - step;
      const bool condition =
          increasing ? (eq ? value(operand1) <= value(operand2)
                           : value(operand1) < value(operand2))
                     : (eq ? value(operand1) > value(operand2)
                           : value(operand1) >= value(operand2));
      last = last && condition;
      if (last)
        result = static_cast<std::uint16_t>(result | (1U << (element * esize)));
      operand1 = (increasing ? operand1 + 1U : operand1 - 1U) & mask;
    }
  }
  s_.p[field(word, 0, 4)] = result;
  s_.nzcv = pred_test(all_true(esize), result, esize);
}

bool Reference::sve(std::uint32_t word) {
  const auto *row = arm64::detail::find_sme_encoding(word);
  if (row == nullptr)
    return false;
  const auto form = row->form;
  const unsigned size = field(word, 22, 2);
  const unsigned esize = 1U << size;
  const unsigned elements = 16U / esize;
  const unsigned d = field(word, 0, 5);
  const unsigned pd = field(word, 0, 4);
  const unsigned n = field(word, 5, 5);
  auto &p = s_.p;
  auto &z = s_.z;
  if (form == forms::ptrue || form == forms::ptrues || form == forms::pfalse) {
    const auto result =
        form == forms::pfalse
            ? std::uint16_t{0}
            : first_elements(esize, pattern_count(field(word, 5, 5), elements));
    p[pd] = result;
    if (form == forms::ptrues)
      s_.nzcv = pred_test(result, result, esize);
    return true;
  }
  if (form == forms::ptest) {
    s_.nzcv = pred_test(p[field(word, 10, 4)], p[field(word, 5, 4)], 1);
    return true;
  }
  if (form == forms::setffr) {
    s_.ffr = 0xffffU;
    return true;
  }
  if (form == forms::wrffr) {
    s_.ffr = p[field(word, 5, 4)];
    return true;
  }
  if (form == forms::rdffr) {
    p[pd] = s_.ffr;
    return true;
  }
  if (form == forms::rdffr_predicated || form == forms::rdffrs) {
    const auto governing = p[field(word, 5, 4)];
    const auto result = static_cast<std::uint16_t>(s_.ffr & governing);
    p[pd] = result;
    if (form == forms::rdffrs)
      s_.nzcv = pred_test(governing, result, 1);
    return true;
  }
  struct Logical final {
    std::uint16_t form;
    bool flags;
  };
  for (const auto [logical, flags] :
       {Logical{forms::and_p, false}, Logical{forms::ands_p, true},
        Logical{forms::bic_p, false}, Logical{forms::bics_p, true},
        Logical{forms::eor_p, false}, Logical{forms::eors_p, true},
        Logical{forms::orr_p, false}, Logical{forms::orrs_p, true},
        Logical{forms::orn_p, false}, Logical{forms::orns_p, true},
        Logical{forms::nand_p, false}, Logical{forms::nands_p, true},
        Logical{forms::nor_p, false}, Logical{forms::nors_p, true},
        Logical{forms::sel_p, false}}) {
    if (form != logical)
      continue;
    const unsigned g = p[field(word, 10, 4)];
    const unsigned a = p[field(word, 5, 4)];
    const unsigned b = p[field(word, 16, 4)];
    unsigned result;
    if (form == forms::and_p || form == forms::ands_p)
      result = g & a & b;
    else if (form == forms::bic_p || form == forms::bics_p)
      result = g & a & ~b;
    else if (form == forms::eor_p || form == forms::eors_p)
      result = g & (a ^ b);
    else if (form == forms::orr_p || form == forms::orrs_p)
      result = g & (a | b);
    else if (form == forms::orn_p || form == forms::orns_p)
      result = g & (a | ~b);
    else if (form == forms::nand_p || form == forms::nands_p)
      result = g & ~(a & b);
    else if (form == forms::nor_p || form == forms::nors_p)
      result = g & ~(a | b);
    else
      result = (a & g) | (b & ~g);
    p[pd] = static_cast<std::uint16_t>(result);
    if (flags)
      s_.nzcv = pred_test(static_cast<std::uint16_t>(g), p[pd], 1);
    return true;
  }
  const auto active_count = [&](std::uint16_t predicate) {
    return static_cast<unsigned>(
        std::popcount(static_cast<unsigned>(predicate & all_true(esize))));
  };
  if (form == forms::cntp) {
    set_x(d, active_count(static_cast<std::uint16_t>(p[field(word, 10, 4)] &
                                                     p[field(word, 5, 4)])));
    return true;
  }
  if (form == forms::incp_scalar || form == forms::decp_scalar) {
    const auto count = active_count(p[field(word, 5, 4)]);
    set_x(d, form == forms::incp_scalar ? x(d) + count : x(d) - count);
    return true;
  }
  if (form == forms::incp_vector || form == forms::decp_vector) {
    const auto count = active_count(p[field(word, 5, 4)]);
    for (unsigned e = 0; e < elements; ++e) {
      const auto old = element_of(z[d], esize, e);
      set_element(z[d], esize, e,
                  form == forms::incp_vector ? old + count : old - count);
    }
    return true;
  }
  const unsigned count = pattern_count(field(word, 5, 5), elements) *
                         (field(word, 16, 4) + 1U);
  if (form == forms::cnt) {
    set_x(d, count);
    return true;
  }
  if (form == forms::inc_scalar || form == forms::dec_scalar) {
    set_x(d, form == forms::inc_scalar ? x(d) + count : x(d) - count);
    return true;
  }
  if (form == forms::inc_vector || form == forms::dec_vector) {
    for (unsigned e = 0; e < elements; ++e) {
      const auto old = element_of(z[d], esize, e);
      set_element(z[d], esize, e,
                  form == forms::inc_vector ? old + count : old - count);
    }
    return true;
  }
  const auto imm6 = arm64::detail::sign_extend(field(word, 5, 6), 6);
  if (form == forms::rdvl) {
    set_x(d, static_cast<std::uint64_t>(imm6 * 16));
    return true;
  }
  if (form == forms::addvl || form == forms::addpl) {
    set_xsp(d, xsp(field(word, 16, 5)) +
                   static_cast<std::uint64_t>(
                       imm6 * (form == forms::addvl ? 16 : 2)));
    return true;
  }
  if (form == forms::movprfx) {
    z[d] = z[n];
    return true;
  }
  if (form == forms::movprfx_predicated) {
    const auto governing = p[field(word, 10, 3)];
    const bool merging = field(word, 16, 1) != 0U;
    Bytes result = z[d];
    for (unsigned e = 0; e < elements; ++e)
      set_element(result, esize, e,
                  active(governing, esize, e)
                      ? element_of(z[n], esize, e)
                      : (merging ? element_of(z[d], esize, e) : 0U));
    z[d] = result;
    return true;
  }
  const auto byte_immediate = [&] {
    return static_cast<std::uint64_t>(
               arm64::detail::sign_extend(field(word, 5, 8), 8))
           << (field(word, 13, 1) * 8U);
  };
  if (form == forms::dup_scalar) {
    z[d] = splat(xsp(n), esize);
    return true;
  }
  if (form == forms::dup_immediate) {
    z[d] = splat(byte_immediate(), esize);
    return true;
  }
  if (form == forms::dup_indexed) {
    const unsigned immediate = (field(word, 22, 2) << 5) | field(word, 16, 5);
    const unsigned log = static_cast<unsigned>(
        std::countr_zero(field(word, 16, 5)));
    const unsigned width = 1U << log;
    const unsigned index = immediate >> (log + 1U);
    Bytes result{};
    if (index < 16U / width)
      for (unsigned e = 0; e < 16U / width; ++e)
        std::memcpy(result.data() + e * width, z[n].data() + index * width,
                    width);
    z[d] = result;
    return true;
  }
  if (form == forms::dupm) {
    const auto value = arm64::detail::decode_logical_immediate(
        true, field(word, 17, 1), field(word, 11, 6), field(word, 5, 6));
    if (!value)
      return false;
    z[d] = splat(*value, 8);
    return true;
  }
  if (form == forms::fdup) {
    z[d] = splat(vfp_expand(field(word, 5, 8), esize * 8U), esize);
    return true;
  }
  if (form == forms::sel_z) {
    const auto governing = p[field(word, 10, 4)];
    const unsigned m = field(word, 16, 5);
    Bytes result{};
    for (unsigned e = 0; e < elements; ++e)
      set_element(result, esize, e,
                  element_of(active(governing, esize, e) ? z[n] : z[m], esize,
                             e));
    z[d] = result;
    return true;
  }
  if (form == forms::and_z || form == forms::orr_z || form == forms::eor_z ||
      form == forms::bic_z) {
    const unsigned m = field(word, 16, 5);
    Bytes result{};
    for (unsigned byte = 0; byte < 16U; ++byte) {
      const unsigned a = z[n][byte];
      const unsigned b = z[m][byte];
      result[byte] = static_cast<std::uint8_t>(
          form == forms::and_z   ? a & b
          : form == forms::orr_z ? a | b
          : form == forms::eor_z ? a ^ b
                                 : a & ~b);
    }
    z[d] = result;
    return true;
  }
  if (form == forms::cpy_merging || form == forms::cpy_zeroing ||
      form == forms::cpy_scalar || form == forms::cpy_simd ||
      form == forms::fcpy) {
    const bool wide_governing = form == forms::cpy_merging ||
                                form == forms::cpy_zeroing ||
                                form == forms::fcpy;
    const auto governing =
        p[wide_governing ? field(word, 16, 4) : field(word, 10, 3)];
    std::uint64_t value;
    if (form == forms::cpy_scalar)
      value = xsp(n);
    else if (form == forms::cpy_simd)
      value = element_of(z[n], esize, 0);
    else if (form == forms::fcpy)
      value = vfp_expand(field(word, 5, 8), esize * 8U);
    else
      value = byte_immediate();
    Bytes result = z[d];
    for (unsigned e = 0; e < elements; ++e)
      if (active(governing, esize, e))
        set_element(result, esize, e, value);
      else if (form == forms::cpy_zeroing)
        set_element(result, esize, e, 0U);
    z[d] = result;
    return true;
  }
  for (const auto candidate :
       {forms::whilelt, forms::whilele, forms::whilelo, forms::whilels,
        forms::whilegt, forms::whilege, forms::whilehi, forms::whilehs,
        forms::whilewr, forms::whilerw})
    if (form == candidate) {
      whiles(word, form);
      return true;
    }
  return false;
}

/* The machine: one executor on one host profile, code on page 0. */

class Machine final {
public:
  Machine(const HostCapabilities &host, std::span<const std::uint32_t> program,
          const State &state)
      : generation_{next_generation_++}, backing_{2},
        mapping_{generation_, code_base, backing_, 2},
        executor_{arm64::JitExecutorOptions{.host_capabilities = host}} {
    for (std::size_t index = 0; index < program.size(); ++index)
      backing_.put32(index * 4U, program[index]);
    put_state(cpu, state);
    require(executor_.begin_execution(generation_, code_base));
  }

  ~Machine() { executor_.end_execution(); }

  /* Runs again from another state, such as another program's PC. */
  void reset(const State &state) { put_state(cpu, state); }

  Machine(const Machine &) = delete;
  Machine &operator=(const Machine &) = delete;

  [[nodiscard]] Event run(std::size_t &retired) {
    retired = 0;
    for (std::size_t round = 0; round < 256U; ++round) {
      std::size_t executed = 0;
      const auto event =
          executor_.run_block(cpu, memory_, generation_, executed);
      retired += executed;
      if (event != Event::continued)
        return event;
    }
    return Event::continued;
  }

  arm64::CpuState cpu{};

private:
  static inline unsigned long next_generation_ = 0x5e00;
  unsigned long generation_;
  JitBacking backing_;
  JitMapping mapping_;
  JitExecutor executor_;
  StaticGuestMemory memory_;
};

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
  unsigned below(unsigned bound) noexcept {
    return static_cast<unsigned>(next() % bound);
  }

  std::uint16_t predicate() noexcept {
    switch (below(6)) {
    case 0:
      return 0;
    case 1:
      return 0xffff;
    case 2:
      return static_cast<std::uint16_t>((1U << below(17)) - 1U);
    case 3:
      return static_cast<std::uint16_t>(0xffffU << below(16));
    default:
      return static_cast<std::uint16_t>(next());
    }
  }

  /* Values near the boundaries WHILE* compares across. */
  std::uint64_t scalar() noexcept {
    static constexpr std::array<std::uint64_t, 8> bases{
        0U,
        0x7fff'fff0U,
        0xffff'fff0U,
        0x7fff'ffff'ffff'fff0ULL,
        0xffff'ffff'ffff'fff0ULL,
        0x8000'0000'0000'0000ULL,
        0xffff'ffff'8000'0000ULL,
        0x1'0000'0000ULL};
    switch (below(4)) {
    case 0:
      return next();
    case 1:
      return below(48);
    default:
      return bases[below(bases.size())] + below(40);
    }
  }

private:
  std::uint64_t state_;
};

[[nodiscard]] State random_state(Random &random) {
  State state;
  for (auto &value : state.x)
    value = random.scalar();
  state.sp = 0x7ff0'0000ULL + random.below(4096) * 16U;
  state.nzcv = static_cast<std::uint32_t>(random.below(16)) << 28;
  for (auto &vector : state.z)
    for (auto &byte : vector)
      byte = static_cast<std::uint8_t>(random.next());
  for (auto &predicate : state.p)
    predicate = random.predicate();
  state.ffr = random.predicate();
  state.pc = code_base;
  return state;
}

/* A word's base and the fields drawn at random. */
struct Shape final {
  std::uint32_t base;
  std::uint16_t form;
  std::array<std::uint8_t, 12> fields; // low, width pairs; width 0 ends
};

[[nodiscard]] std::vector<Shape> shapes() {
  using F = std::array<std::uint8_t, 12>;
  return {
      {0x2518'e000U, forms::ptrue, F{22, 2, 5, 5, 0, 4}},
      {0x2519'e000U, forms::ptrues, F{22, 2, 5, 5, 0, 4}},
      {0x2518'e400U, forms::pfalse, F{0, 4}},
      {0x2550'c000U, forms::ptest, F{10, 4, 5, 4}},
      {0x2500'4000U, forms::and_p, F{16, 4, 10, 4, 5, 4, 0, 4}},
      {0x2540'4000U, forms::ands_p, F{16, 4, 10, 4, 5, 4, 0, 4}},
      {0x2500'4010U, forms::bic_p, F{16, 4, 10, 4, 5, 4, 0, 4}},
      {0x2540'4010U, forms::bics_p, F{16, 4, 10, 4, 5, 4, 0, 4}},
      {0x2500'4200U, forms::eor_p, F{16, 4, 10, 4, 5, 4, 0, 4}},
      {0x2540'4200U, forms::eors_p, F{16, 4, 10, 4, 5, 4, 0, 4}},
      {0x2580'4000U, forms::orr_p, F{16, 4, 10, 4, 5, 4, 0, 4}},
      {0x25c0'4000U, forms::orrs_p, F{16, 4, 10, 4, 5, 4, 0, 4}},
      {0x2580'4010U, forms::orn_p, F{16, 4, 10, 4, 5, 4, 0, 4}},
      {0x25c0'4010U, forms::orns_p, F{16, 4, 10, 4, 5, 4, 0, 4}},
      {0x2580'4210U, forms::nand_p, F{16, 4, 10, 4, 5, 4, 0, 4}},
      {0x25c0'4210U, forms::nands_p, F{16, 4, 10, 4, 5, 4, 0, 4}},
      {0x2580'4200U, forms::nor_p, F{16, 4, 10, 4, 5, 4, 0, 4}},
      {0x25c0'4200U, forms::nors_p, F{16, 4, 10, 4, 5, 4, 0, 4}},
      {0x2500'4210U, forms::sel_p, F{16, 4, 10, 4, 5, 4, 0, 4}},
      {0x2520'8000U, forms::cntp, F{22, 2, 10, 4, 5, 4, 0, 5}},
      {0x252c'8800U, forms::incp_scalar, F{22, 2, 5, 4, 0, 5}},
      {0x252d'8800U, forms::decp_scalar, F{22, 2, 5, 4, 0, 5}},
      {0x256c'8000U, forms::incp_vector, F{23, 1, 5, 4, 0, 5}},
      {0x25ac'8000U, forms::incp_vector, F{22, 1, 5, 4, 0, 5}},
      {0x256d'8000U, forms::decp_vector, F{23, 1, 5, 4, 0, 5}},
      {0x0420'e000U, forms::cnt, F{22, 2, 16, 4, 5, 5, 0, 5}},
      {0x0430'e000U, forms::inc_scalar, F{22, 2, 16, 4, 5, 5, 0, 5}},
      {0x0430'e400U, forms::dec_scalar, F{22, 2, 16, 4, 5, 5, 0, 5}},
      {0x0470'c000U, forms::inc_vector, F{23, 1, 16, 4, 5, 5, 0, 5}},
      {0x04f0'c400U, forms::dec_vector, F{22, 1, 16, 4, 5, 5, 0, 5}},
      {0x04bf'5000U, forms::rdvl, F{5, 6, 0, 5}},
      {0x0420'5000U, forms::addvl, F{16, 5, 5, 6, 0, 5}},
      {0x0460'5000U, forms::addpl, F{16, 5, 5, 6, 0, 5}},
      {0x252c'9000U, forms::setffr, F{}},
      {0x2519'f000U, forms::rdffr, F{0, 4}},
      {0x2518'f000U, forms::rdffr_predicated, F{5, 4, 0, 4}},
      {0x2558'f000U, forms::rdffrs, F{5, 4, 0, 4}},
      {0x2528'9000U, forms::wrffr, F{5, 4}},
      {0x0420'bc00U, forms::movprfx, F{5, 5, 0, 5}},
      {0x0410'2000U, forms::movprfx_predicated,
       F{22, 2, 16, 1, 10, 3, 5, 5, 0, 5}},
      {0x0520'3800U, forms::dup_scalar, F{22, 2, 5, 5, 0, 5}},
      {0x2538'c000U, forms::dup_immediate, F{5, 8, 0, 5}},
      {0x2578'c000U, forms::dup_immediate, F{23, 1, 13, 1, 5, 8, 0, 5}},
      {0x0520'2000U, forms::dup_indexed, F{22, 2, 16, 5, 5, 5, 0, 5}},
      {0x05c0'0000U, forms::dupm, F{5, 13, 0, 5}},
      {0x2579'c000U, forms::fdup, F{23, 1, 5, 8, 0, 5}},
      {0x25b9'c000U, forms::fdup, F{22, 1, 5, 8, 0, 5}},
      {0x0510'4000U, forms::cpy_merging, F{16, 4, 5, 8, 0, 5}},
      {0x0550'4000U, forms::cpy_merging, F{23, 1, 16, 4, 13, 1, 5, 8, 0, 5}},
      {0x0510'0000U, forms::cpy_zeroing, F{16, 4, 5, 8, 0, 5}},
      {0x0550'0000U, forms::cpy_zeroing, F{23, 1, 16, 4, 13, 1, 5, 8, 0, 5}},
      {0x0528'a000U, forms::cpy_scalar, F{22, 2, 10, 3, 5, 5, 0, 5}},
      {0x0520'8000U, forms::cpy_simd, F{22, 2, 10, 3, 5, 5, 0, 5}},
      {0x0550'c000U, forms::fcpy, F{23, 1, 16, 4, 5, 8, 0, 5}},
      {0x0590'c000U, forms::fcpy, F{22, 1, 16, 4, 5, 8, 0, 5}},
      {0x0520'c000U, forms::sel_z, F{22, 2, 16, 5, 10, 4, 5, 5, 0, 5}},
      {0x0420'3000U, forms::and_z, F{16, 5, 5, 5, 0, 5}},
      {0x0460'3000U, forms::orr_z, F{16, 5, 5, 5, 0, 5}},
      {0x04a0'3000U, forms::eor_z, F{16, 5, 5, 5, 0, 5}},
      {0x04e0'3000U, forms::bic_z, F{16, 5, 5, 5, 0, 5}},
      {0x2520'0400U, forms::whilelt, F{22, 2, 16, 5, 12, 1, 5, 5, 0, 4}},
      {0x2520'0410U, forms::whilele, F{22, 2, 16, 5, 12, 1, 5, 5, 0, 4}},
      {0x2520'0c00U, forms::whilelo, F{22, 2, 16, 5, 12, 1, 5, 5, 0, 4}},
      {0x2520'0c10U, forms::whilels, F{22, 2, 16, 5, 12, 1, 5, 5, 0, 4}},
      {0x2520'0010U, forms::whilegt, F{22, 2, 16, 5, 12, 1, 5, 5, 0, 4}},
      {0x2520'0000U, forms::whilege, F{22, 2, 16, 5, 12, 1, 5, 5, 0, 4}},
      {0x2520'0810U, forms::whilehi, F{22, 2, 16, 5, 12, 1, 5, 5, 0, 4}},
      {0x2520'0800U, forms::whilehs, F{22, 2, 16, 5, 12, 1, 5, 5, 0, 4}},
      {0x2520'3000U, forms::whilewr, F{22, 2, 16, 5, 5, 5, 0, 4}},
      {0x2520'3010U, forms::whilerw, F{22, 2, 16, 5, 5, 5, 0, 4}},
  };
}

[[nodiscard]] std::uint32_t draw(Random &random, const Shape &shape) {
  for (unsigned attempt = 0;; ++attempt) {
    require(attempt < 64U);
    std::uint32_t word = shape.base;
    for (std::size_t index = 0; index + 1U < shape.fields.size(); index += 2) {
      const unsigned width = shape.fields[index + 1U];
      if (width == 0U)
        break;
      word |= (static_cast<std::uint32_t>(random.next()) & ((1U << width) - 1U))
              << shape.fields[index];
    }
    if (shape.form == forms::dupm &&
        !arm64::detail::decode_logical_immediate(
            true, field(word, 17, 1), field(word, 11, 6), field(word, 5, 6)))
      continue;
    /* A field drawn outside its form, such as a zero tsz, draws again. */
    const auto *row = arm64::detail::find_sme_encoding(word);
    if (row == nullptr || row->form != shape.form)
      continue;
    return word;
  }
}

/* The flag-setting forms, which a B.cond may follow. */
[[nodiscard]] bool sets_flags(std::uint16_t form) {
  for (const auto candidate :
       {forms::ptrues, forms::ptest, forms::ands_p, forms::bics_p,
        forms::eors_p, forms::orrs_p, forms::orns_p, forms::nands_p,
        forms::nors_p, forms::rdffrs, forms::whilelt, forms::whilele,
        forms::whilelo, forms::whilels, forms::whilegt, forms::whilege,
        forms::whilehi, forms::whilehs, forms::whilewr, forms::whilerw})
    if (form == candidate)
      return true;
  return false;
}

/* Runs `program` from `initial` on the JIT and the reference; true when
 * they agree. */
[[nodiscard]] bool agree(const HostCapabilities &host,
                         std::span<const std::uint32_t> program,
                         const State &initial, const char *label) {
  Reference reference{initial};
  require(reference.run(program));
  Machine machine{host, program, initial};
  machine.cpu.sve_live = 0;
  std::size_t retired = 0;
  const auto event = machine.run(retired);
  const auto got = get_state(machine.cpu);
  const bool same = event == Event::system_call && got == reference.state() &&
                    retired == reference.retired() && machine.cpu.sve_live == 1U;
  if (!same) {
    std::fprintf(stderr, "%s: event %d retired %zu (expected %zu) live %u:",
                 label, static_cast<int>(event), retired, reference.retired(),
                 machine.cpu.sve_live);
    for (const auto word : program)
      std::fprintf(stderr, " %08x", word);
    std::fprintf(stderr, "\n");
    print_state_difference(got, reference.state());
  }
  return same;
}

/* Every form, alone and in random programs that follow flag-setting forms
 * with B.cond, beside the reference. */
void random_programs_match_the_reference(const HostCapabilities &host,
                                         const char *label) {
  const auto all = shapes();
  Random random{0x5e4c'0000ULL ^ host.sme_svl_bytes};
  std::size_t failures = 0;
  for (const auto &shape : all)
    for (unsigned round = 0; round < 24U; ++round) {
      const std::array program{draw(random, shape), svc};
      failures += agree(host, program, random_state(random), label) ? 0U : 1U;
    }
  for (unsigned round = 0; round < 1500U && failures < 8U; ++round) {
    std::vector<std::uint32_t> program;
    const unsigned length = 2U + random.below(12);
    while (program.size() < length) {
      const auto &shape = all[random.below(all.size())];
      program.push_back(draw(random, shape));
      if (sets_flags(shape.form) && random.below(2) == 0U)
        program.push_back(b_cond(random.below(14), 2));
    }
    program.push_back(svc);
    program.push_back(svc);
    failures += agree(host, program, random_state(random), label) ? 0U : 1U;
  }
  require(failures == 0U);
}

/* A WHILELO loop: the fused WHILE and B.NONE end each iteration's block. */
void fused_loop_counts_iterations(const HostCapabilities &host) {
  /* for (x1 = 0; whilelo p0.s, x1, x2; x1 += 4) ++x3; */
  const std::array program{whilelo(0, 2, 1, 2), b_cond(0, 4), incw(1),
                           add_immediate(3, 3, 1), branch(-4), svc};
  for (const std::uint64_t limit : {0U, 1U, 3U, 4U, 5U, 37U, 64U}) {
    State initial{};
    initial.x[1] = 0;
    initial.x[2] = limit;
    initial.x[3] = 0;
    initial.pc = code_base;
    require(agree(host, program, initial, "fused loop"));
    Reference reference{initial};
    require(reference.run(program));
    require(reference.state().x[3] == (limit + 3U) / 4U);
  }
  /* Every condition after every WHILE* form, both ways. */
  Random random{0x1f0b'0000ULL};
  for (unsigned condition = 0; condition < 14U; ++condition)
    for (const auto &shape : shapes()) {
      if (!sets_flags(shape.form) || shape.form == forms::ptest)
        continue;
      for (unsigned round = 0; round < 6U; ++round) {
        const std::array program{draw(random, shape), b_cond(condition, 2),
                                 add_immediate(4, 4, 1), svc, svc};
        require(agree(host, program, random_state(random), "fusion"));
      }
    }
}

/* Every WHILE* form, element size and operand width, with XZR as either
 * operand too, across operand pairs at and around the bounds where a
 * comparison wraps or a distance saturates. */
void whiles_match_at_the_bounds(const HostCapabilities &host) {
  std::vector<std::uint32_t> code;
  for (const auto &shape : shapes()) {
    bool is_while = false;
    for (const auto form :
         {forms::whilelt, forms::whilele, forms::whilelo, forms::whilels,
          forms::whilegt, forms::whilege, forms::whilehi, forms::whilehs,
          forms::whilewr, forms::whilerw})
      is_while = is_while || shape.form == form;
    if (!is_while)
      continue;
    const bool conflict =
        shape.form == forms::whilewr || shape.form == forms::whilerw;
    for (unsigned size = 0; size < 4U; ++size)
      for (unsigned wide = conflict ? 1U : 0U; wide < 2U; ++wide)
        for (const auto [n, m] : {std::pair{1U, 2U}, std::pair{31U, 2U},
                                  std::pair{1U, 31U}}) {
          code.push_back(shape.base | (size << 22) | (m << 16) |
                         (conflict ? 0U : wide << 12) | (n << 5) | 7U);
          code.push_back(svc);
        }
  }
  constexpr std::array<std::uint64_t, 20> bounds{
      0U,
      1U,
      2U,
      15U,
      16U,
      17U,
      0x7fff'fffeU,
      0x7fff'ffffU,
      0x8000'0000U,
      0xffff'fffeU,
      0xffff'ffffU,
      0x1'0000'0000ULL,
      0xffff'ffff'8000'0000ULL,
      0x7fff'ffff'ffff'fffeULL,
      0x7fff'ffff'ffff'ffffULL,
      0x8000'0000'0000'0000ULL,
      0x8000'0000'0000'0001ULL,
      0xffff'ffff'ffff'fff0ULL,
      0xffff'ffff'ffff'fffeULL,
      0xffff'ffff'ffff'ffffULL};
  std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
  for (const auto first : bounds) {
    for (const auto second : bounds)
      pairs.emplace_back(first, second);
    for (const std::int64_t delta : {-17, -16, -15, -2, -1, 1, 15, 16, 17})
      pairs.emplace_back(first, first + static_cast<std::uint64_t>(delta));
  }
  Machine machine{host, code, State{.pc = code_base}};
  std::size_t failures = 0;
  for (std::size_t at = 0; at < code.size(); at += 2U)
    for (const auto &[first, second] : pairs) {
      State initial{};
      initial.x[1] = first;
      initial.x[2] = second;
      initial.nzcv = 0x1000'0000U;
      initial.p[7] = 0x5a5aU;
      initial.pc = code_base + at * 4U;
      Reference reference{initial};
      require(reference.run(code));
      machine.reset(initial);
      std::size_t retired = 0;
      const auto event = machine.run(retired);
      const auto got = get_state(machine.cpu);
      if (event != Event::system_call || retired != 2U ||
          got != reference.state()) {
        std::fprintf(stderr, "%08x x1 %#llx x2 %#llx: event %d\n", code[at],
                     static_cast<unsigned long long>(first),
                     static_cast<unsigned long long>(second),
                     static_cast<int>(event));
        print_state_difference(got, reference.state());
        require(++failures < 8U);
      }
    }
  require(failures == 0U);
}

/* A block holding an SVE word takes the FP-access path first, so Z reads
 * the task's deferred image, and marks the task live; one without leaves
 * the marker alone. */
void fp_access_and_live_marker(const HostCapabilities &host) {
  std::array<std::byte, 32 * 16> image{};
  for (std::size_t index = 0; index < image.size(); ++index)
    image[index] = static_cast<std::byte>(index * 13U + 7U);
  const auto deferred = [&](Machine &machine) {
    machine.cpu.fp_loaded = false;
    machine.cpu.defer_fp_load(image.data(), 0, 0, ~0U);
    machine.cpu.sve_live = 0;
  };
  {
    const std::array program{dup_indexed(0, 1, 4, 2), svc};
    Machine machine{host, program, State{.pc = code_base}};
    deferred(machine);
    std::size_t retired = 0;
    require(machine.run(retired) == Event::system_call && retired == 2U);
    require(machine.cpu.fp_loaded && machine.cpu.sve_live == 1U);
    const auto state = get_state(machine.cpu);
    for (unsigned lane = 0; lane < 4U; ++lane)
      require(std::memcmp(state.z[0].data() + lane * 4U,
                          image.data() + 16U + 8U, 4) == 0);
  }
  {
    /* Only P: the block still makes the FP state current. */
    const std::array program{add_immediate(0, 0, 1), ptrue(3, 0), svc};
    Machine machine{host, program, State{.pc = code_base}};
    deferred(machine);
    std::size_t retired = 0;
    require(machine.run(retired) == Event::system_call && retired == 3U);
    require(machine.cpu.fp_loaded && machine.cpu.sve_live == 1U);
    require(arm64::sve_predicate_packed(machine.cpu.sve_p[3]) == 0xffffU);
  }
  {
    const std::array program{add_immediate(0, 0, 1), svc};
    Machine machine{host, program, State{.pc = code_base}};
    machine.cpu.sve_live = 0;
    std::size_t retired = 0;
    require(machine.run(retired) == Event::system_call &&
            machine.cpu.sve_live == 0U);
  }
}

/* A reserved encoding is the illegal-instruction exit, at its own PC,
 * after the block's earlier words retire: a DUPM, AND, ORR or EOR
 * immediate the pseudocode rejects, and a shifted byte immediate. */
void reserved_words_are_illegal(const HostCapabilities &host) {
  for (const std::uint32_t word :
       {0x05c0'07e0U,   // dupm with a reserved immediate
        0x0580'07e0U,   // and z0.s, z0.s, #imm with a reserved immediate
        0x0500'07e0U,   // orr, likewise
        0x0540'07e0U,   // eor, likewise
        0x2538'e000U}) { // dup z0.b, #0, lsl #8
    /* The table may allocate the row (the bitmask immediates) or leave
     * the word without one (the shifted byte immediate). */
    const auto *row = arm64::detail::find_sme_encoding(word);
    require(row == nullptr ||
            arm64::jit::sve_word_reserved(row->form, word));
    const std::array program{ptrue(1, 0), word, svc};
    State initial{};
    initial.pc = code_base;
    Machine machine{host, program, initial};
    std::size_t retired = 0;
    const auto event = machine.run(retired);
    if (event != Event::illegal_instruction || machine.cpu.pc != code_base + 4U ||
        retired != 1U ||
        arm64::sve_predicate_packed(machine.cpu.sve_p[1]) != 0xffffU) {
      std::fprintf(stderr, "%08x: event %d pc %#llx retired %zu\n", word,
                   static_cast<int>(event),
                   static_cast<unsigned long long>(machine.cpu.pc), retired);
      require(false);
    }
  }
}

/* Code checks. */

[[nodiscard]] jit_test::CompiledCode compile(const HostCapabilities &host,
                                             std::span<const std::uint32_t> words,
                                             std::uint8_t mode = 0,
                                             jit_test::LinkShape shape =
                                                 jit_test::LinkShape::unlinked) {
  const uml::cpu::HostCapabilitiesScope scope{host};
  return jit_test::compile_block_in_mode(code_base, words, mode, shape).code;
}

[[nodiscard]] bool holds_sve_word(const jit_test::CompiledCode &code) {
  for (std::size_t index = 0; index < code.executable_words; ++index)
    if (arm64::detail::sme_encoding_space(code.words[index]))
      return true;
  return false;
}

/* For SM=0, a row of the profile translates unless its encoding is
 * reserved, and then holds no SVE word: a control-core row accesses the FP
 * state, any other row ends the block in a call to the semantic library.
 * A row the virtual CPU lacks, or one that needs ZA while it is clear, is
 * the illegal exit. For SM=1, a translation is the same with the SVE
 * profile and without it, and so is one without an SVE word for SM=0. */
void rows_translate_where_the_profile_runs() {
  const auto with = sve_host();
  const auto with_sme = sve_sme_host();
  const auto without_sme = sve_sme_host(false);
  std::size_t inline_rows = 0;
  std::size_t library_rows = 0;
  std::size_t illegal = 0;
  std::size_t streaming = 0;
  for (const auto &row : arm64::detail::sme_encodings) {
    const std::array program{row.value, svc};
    const bool sve_row =
        (row.non_streaming_requirement & arm64::detail::sme_feature_sve) != 0U;
    if (sve_row) {
      for (const auto *host : {&with, &with_sme}) {
        const auto features = arm64::implemented_sve_features(*host);
        const bool implemented =
            (row.non_streaming_requirement & features) ==
                row.non_streaming_requirement &&
            (row.mode & arm64::detail::sme_mode_za) == 0U;
        const bool runs =
            implemented && !arm64::jit::sve_word_reserved(row.form, row.value);
        const bool inlined = runs && arm64::jit::implements_sve_form(row.form);
        /* Every implemented row is one the library covers. */
        if (implemented && !arm64::detail::sve_semantics_covers(row)) {
          std::fprintf(stderr, "row %08x form %u: the library lacks it\n",
                       row.value, row.form);
          require(false);
        }
        for (const auto shape :
             {jit_test::LinkShape::unlinked, jit_test::LinkShape::tracked}) {
          const auto code = compile(*host, program, 0, shape);
          if (code.guest_illegal == runs || holds_sve_word(code) ||
              (inlined && !code.accesses_fp_state)) {
            std::fprintf(stderr, "row %08x form %u: illegal %d sve word %d\n",
                         row.value, row.form, code.guest_illegal,
                         holds_sve_word(code));
            require(false);
          }
        }
        if (host != &with)
          continue;
        if (!runs)
          ++illegal;
        else if (inlined)
          ++inline_rows;
        else
          ++library_rows;
      }
    }
    for (const std::uint8_t mode :
         {arm64::svcr_sm, static_cast<std::uint8_t>(arm64::svcr_mask)}) {
      if (compile(with_sme, program, mode).words !=
          compile(without_sme, program, mode).words) {
        std::fprintf(stderr, "row %08x differs for mode %u\n", row.value,
                     mode);
        require(false);
      }
      ++streaming;
    }
    /* A host without the profile translates no SVE word. */
    if (sve_row && !compile(sve_host(false), program).guest_illegal) {
      std::fprintf(stderr, "row %08x translates without SVE\n", row.value);
      require(false);
    }
  }
  std::printf("rows: %zu inline, %zu library, %zu illegal; %zu streaming "
              "compared\n",
              inline_rows, library_rows, illegal, streaming);
  require(inline_rows > 60U && library_rows > 300U && streaming > 2000U);
  /* Every immediate of the bitmask forms agrees with the decoder. */
  for (const std::uint32_t base :
       {0x0500'0000U, 0x0540'0000U, 0x0580'0000U, 0x05c0'0000U})
    for (std::uint32_t immediate = 0; immediate < 0x2000U; immediate += 7U) {
      const std::array program{base | (immediate << 5), svc};
      const bool reserved =
          !arm64::detail::decode_logical_immediate(
              true, field(immediate, 12, 1), field(immediate, 6, 6),
              field(immediate, 0, 6));
      require(compile(with, program).guest_illegal == reserved);
    }
  /* Blocks without an SVE word. */
  for (const auto &program : std::initializer_list<std::vector<std::uint32_t>>{
           {add_immediate(0, 1, 5), 0x8b02'0020U, svc},
           {0x4ea2'1c20U, 0x6e22'8420U, 0x3dc0'0020U, svc},
           {0xf940'0020U, 0xb100'041fU, b_cond(1, 3), 0xd503'201fU, svc, svc},
           {0x1e60'2820U, 0x0e20'8400U, 0x4f00'0400U, svc}})
    for (const std::uint8_t mode : {std::uint8_t{0}, arm64::svcr_za})
      require(compile(with_sme, program, mode).words ==
              compile(without_sme, program, mode).words);
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  rows_translate_where_the_profile_runs();
  if (!jit_available()) {
    std::printf("run-time checks skipped: the JIT does not run here\n");
    return 0;
  }
  for (const auto &[host, label] :
       {std::pair{sve_host(), "sve"}, std::pair{sve_sme_host(), "sve+sme"}}) {
    fp_access_and_live_marker(host);
    reserved_words_are_illegal(host);
    whiles_match_at_the_bounds(host);
    fused_loop_counts_iterations(host);
    random_programs_match_the_reference(host, label);
  }
  return 0;
}
