// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Streaming JIT translations, checked as code on injected SME profiles: no
 * AdvSIMD or other FP&SIMD word reaches an SM=1 translation, the host enters
 * streaming mode only in the external prologue and leaves it before every
 * host call and return, and only SM=1 translations touch the host's
 * streaming state: no other translation holds an SVE or SME word. No SM=1
 * translation reads or writes the host's FPCR or FPSR or takes the native FP
 * environment, since XNU resets FPSR on any kernel entry in streaming mode.
 * Every
 * witness word of the A64 evidence table, which the registration passes as
 * the only argument, is compiled in every mode and link shape.
 */
#include "runtime_test_route.hpp"
#include "require.hpp"
#include "cpu/arm64/jit/jit_emitter.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_semantic_memory.hpp"
#include "cpu/arm64/jit/jit_sme_transition.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/host_capabilities.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace arm64 = uml::cpu::arm64;
namespace jit_test = uml::cpu::arm64::jit::testing;
using uml::testing::require;

constexpr std::uint32_t system_call = 0xd400'0001U;
constexpr std::uint32_t add_x0_one = 0x9100'0400U;
constexpr std::uint32_t blr_x16 = 0xd63f'0200U;
constexpr std::uint32_t ret = 0xd65f'03c0U;
constexpr std::uint32_t reload_cpu = 0xf940'0befU; // ldr x15, [sp, #16]

struct Witness final {
  std::string id;
  std::uint32_t word{};
};

[[nodiscard]] std::vector<Witness> witnesses(const char *bindings) {
  std::ifstream table{bindings};
  require(static_cast<bool>(table));
  std::vector<Witness> result;
  std::string line;
  std::getline(table, line); // header
  while (std::getline(table, line)) {
    std::istringstream fields{line};
    std::array<std::string, 6> column;
    for (auto &field : column)
      require(static_cast<bool>(std::getline(fields, field, '\t')));
    result.push_back({column[0], static_cast<std::uint32_t>(
                                     std::stoul(column[5], nullptr, 16))});
  }
  require(result.size() > 2000U);
  return result;
}

/* The host's own profile with the SME2 (or SME1-only) profile at `svl`,
 * or without SME at svl 0, and without FEAT_CSSC, so CNT takes its
 * fallback as on the M4. */
[[nodiscard]] uml::cpu::HostCapabilities sme_host(unsigned svl,
                                                  bool sme2 = true) {
  auto host = uml::cpu::host_capabilities();
  for (const auto feature :
       {uml::cpu::ArmFeature::sme, uml::cpu::ArmFeature::sme2,
        uml::cpu::ArmFeature::sme_f64f64, uml::cpu::ArmFeature::sme_i16i64})
    host.supported.set(static_cast<std::size_t>(feature), svl != 0U);
  if (!sme2)
    host.supported.reset(static_cast<std::size_t>(uml::cpu::ArmFeature::sme2));
  host.supported.reset(static_cast<std::size_t>(uml::cpu::ArmFeature::cssc));
  host.cssc = false;
  host.sme_svl_bytes = static_cast<std::uint16_t>(svl);
  return host;
}

/* An FP&SIMD data-processing word, or a load or store of SIMD&FP
 * registers. */
[[nodiscard]] constexpr bool fp_simd_word(std::uint32_t word) noexcept {
  const auto op0 = (word >> 25U) & 15U;
  return op0 == 7U || op0 == 15U || (word & 0x0e00'0000U) == 0x0c00'0000U;
}

/* MRS or MSR of FPCR or FPSR. */
[[nodiscard]] constexpr bool fp_control_access(std::uint32_t word) noexcept {
  switch (word & 0xffff'ffe0U) {
  case 0xd53b'4400U:
  case 0xd51b'4400U:
  case 0xd53b'4420U:
  case 0xd51b'4420U:
    return true;
  default:
    return false;
  }
}

enum class Callee {
  enter,
  leave,
  streaming_bridge,
  preserving_bridge,
  helper,
};

[[nodiscard]] std::uintptr_t address(const void *function) noexcept {
  return arm64::callable_address(function);
}

/* The callee of the BLR X16 at `index`, whose LDR X16 (literal) is the
 * word before it. */
[[nodiscard]] Callee callee(const jit_test::CompiledCode &code,
                            std::size_t index) {
  require(index != 0U);
  const auto load = code.words[index - 1U];
  require((load & 0xff00'001fU) == 0x5800'0010U);
  const auto offset = static_cast<std::int64_t>(
      static_cast<std::int32_t>(((load >> 5U) & 0x7ffffU) << 13U) >> 13U);
  const auto cell =
      static_cast<std::size_t>(static_cast<std::int64_t>(index - 1U) + offset);
  require(cell >= code.executable_words && cell + 1U < code.words.size());
  const auto target = code.words[cell] |
                      (static_cast<std::uint64_t>(code.words[cell + 1U])
                       << 32U);
  if (target ==
      address(reinterpret_cast<const void *>(&uml_arm64_jit_sme_enter)))
    return Callee::enter;
  if (target ==
      address(reinterpret_cast<const void *>(&uml_arm64_jit_sme_leave)))
    return Callee::leave;
  if (target == address(reinterpret_cast<const void *>(
                    &uml_arm64_jit_tlb_translate_streaming)))
    return Callee::streaming_bridge;
  if (target == address(reinterpret_cast<const void *>(
                    &arm64::uml_arm64_jit_tlb_translate_preserving)))
    return Callee::preserving_bridge;
  return Callee::helper;
}

struct Checked final {
  std::size_t translations{};
  std::size_t helper_calls{};
};

void check_translation(const jit_test::CompiledCode &code, std::uint8_t mode,
                       const std::string &label, Checked &checked) {
  if (code.words.empty())
    return;
  require(code.executable_words != 0U &&
          code.executable_words <= code.words.size());
  const bool streaming = (mode & arm64::svcr_sm) != 0U;
  if (streaming && code.native_fp) {
    std::fprintf(stderr, "%s mode %u: takes the native FP environment\n",
                 label.c_str(), mode);
    require(false);
  }
  std::size_t enters = 0;
  /* Translated code after a helper would run outside streaming mode with
   * the guest's state spilled, so every helper must be terminal: the next
   * call after it is the epilogue's leave. */
  bool after_helper = false;
  for (std::size_t index = 0; index < code.executable_words; ++index) {
    const auto word = code.words[index];
    if (!streaming && arm64::detail::sme_encoding_space(word)) {
      std::fprintf(stderr, "%s mode %u: SVE or SME word %08x at %zu\n",
                   label.c_str(), mode, word, index);
      require(false);
    }
    if (streaming && (fp_simd_word(word) || fp_control_access(word))) {
      std::fprintf(stderr, "%s mode %u: FP&SIMD, FPCR or FPSR word %08x at %zu\n",
                   label.c_str(), mode, word, index);
      require(false);
    }
    if (word == ret && streaming) {
      /* Every epilogue leaves streaming mode before restoring the frame,
       * with the vCPU pointer reloaded, since a helper may have changed
       * x15. */
      require(index >= 7U && code.words[index - 5U] == blr_x16);
      require(callee(code, index - 5U) == Callee::leave);
      require(code.words[index - 7U] == reload_cpu);
    }
    if (word != blr_x16)
      continue;
    const auto target = callee(code, index);
    if (streaming && after_helper) {
      require(target == Callee::leave);
      after_helper = false;
    }
    if (!streaming) {
      require(target == Callee::helper ||
              target == Callee::preserving_bridge);
      continue;
    }
    switch (target) {
    case Callee::enter:
      ++enters;
      require(index < code.internal_entry_word);
      break;
    case Callee::leave:
    case Callee::streaming_bridge:
      break;
    case Callee::preserving_bridge:
      require(false);
      break;
    case Callee::helper:
      /* A host helper call follows its own leave sequence. */
      require(index >= 3U && code.words[index - 2U] == blr_x16 &&
              callee(code, index - 2U) == Callee::leave);
      ++checked.helper_calls;
      after_helper = true;
      break;
    }
  }
  require(enters == (streaming ? 1U : 0U));
  require(!after_helper);
  ++checked.translations;
}

void check_program(std::uint64_t pc, std::span<const std::uint32_t> program,
                   const std::string &label, Checked &checked) {
  for (std::uint8_t mode = 0; mode <= arm64::svcr_mask; ++mode)
    for (const auto shape :
         {jit_test::LinkShape::unlinked, jit_test::LinkShape::direct,
          jit_test::LinkShape::indirect, jit_test::LinkShape::tracked})
      check_translation(
          jit_test::compile_block_in_mode(pc, program, mode, shape).code,
          mode, label, checked);
}

void streaming_translations_hold_no_fp_simd_word(const char *bindings) {
  const auto host = sme_host(64);
  const uml::cpu::HostCapabilitiesScope scope{host};
  constexpr std::uint64_t pc = 0x4f00'0000U;
  Checked checked;
  for (const auto &witness : witnesses(bindings)) {
    const std::array alone{witness.word, system_call};
    const std::array after{add_x0_one, witness.word, system_call};
    check_program(pc, alone, witness.id, checked);
    check_program(pc, after, witness.id, checked);
  }
  /* Sequences the compiler lowers as one unit: FEAT_MOPS prologue, main
   * and epilogue triples, exclusive pairs, unsigned-offset runs and CNT. */
  const std::vector<std::vector<std::uint32_t>> sequences{
      {0x1901'0440U, 0x1941'0440U, 0x1981'0440U, system_call}, // cpyfp/m/e
      {0x19c2'0420U, 0x19c2'4420U, 0x19c2'8420U, system_call}, // setp/m/e
      {0xc85f'7c20U, 0xc802'7c20U, system_call},               // ldxr/stxr
      {0xf940'0020U, 0xf940'0421U, 0xf940'0822U, system_call}, // ldr run
      {0xf900'03e0U, 0xf900'07e1U, system_call},               // str sp run
      {0x3dc0'0020U, 0x3dc0'0421U, system_call},               // ldr q run
      {0xdac0'1c20U, 0x5ac0'1c20U, system_call},               // cnt x, w
      {0xd51b'4421U, 0xd53b'4422U, system_call},               // fpsr
      {0xd51b'4401U, 0xd53b'4402U, system_call},               // fpcr
  };
  for (const auto &sequence : sequences)
    check_program(pc, sequence, "sequence", checked);
  std::fprintf(stderr, "checked %zu translations, %zu helper calls\n",
               checked.translations, checked.helper_calls);
  require(checked.translations > 70'000U);
  require(checked.helper_calls != 0U);
}

/* Every translation, in every mode, is the same at every SVL: whatever
 * depends on it is read at run time. */
void translations_do_not_depend_on_the_svl(const char *bindings) {
  constexpr std::uint64_t pc = 0x4f20'0000U;
  const auto all = witnesses(bindings);
  std::vector<std::vector<std::uint32_t>> reference;
  {
    const auto host = sme_host(64);
    const uml::cpu::HostCapabilitiesScope scope{host};
    for (const auto &witness : all)
      for (std::uint8_t mode = 0; mode <= arm64::svcr_mask; ++mode) {
        const std::array program{witness.word, system_call};
        reference.push_back(
            jit_test::compile_block_in_mode(pc, program, mode).code.words);
      }
  }
  for (const unsigned svl : {16U, 32U, 128U, 256U}) {
    const auto host = sme_host(svl);
    const uml::cpu::HostCapabilitiesScope scope{host};
    std::size_t index = 0;
    for (const auto &witness : all)
      for (std::uint8_t mode = 0; mode <= arm64::svcr_mask; ++mode) {
        const std::array program{witness.word, system_call};
        const auto words =
            jit_test::compile_block_in_mode(pc, program, mode).code.words;
        if (words != reference[index]) {
          std::fprintf(stderr, "%s mode %u differs at SVL %u\n",
                       witness.id.c_str(), mode, svl);
          require(false);
        }
        ++index;
      }
  }
}

[[nodiscard]] bool illegal_in(std::uint32_t word, std::uint8_t mode) {
  const std::array program{word};
  return jit_test::compile_block_in_mode(0x4f30'0000U, program, mode)
      .code.guest_illegal;
}

/* The control core translates on the SME profile, each form only in the
 * modes its checks allow, and nothing of it without the profile. */
void control_core_follows_profile_and_mode() {
  struct Form final {
    std::uint32_t word;
    bool needs_za;
    bool needs_sme2;
  };
  constexpr std::array control{
      Form{0x04bf'5823U, false, false}, // rdsvl x3, #1
      Form{0x0424'5fe3U, false, false}, // addsvl x3, x4, #-1
      Form{0x047f'585fU, false, false}, // addspl sp, sp, #2
      Form{0xd503'477fU, false, false}, // smstart
      Form{0xd503'467fU, false, false}, // smstop
      Form{0xd503'437fU, false, false}, // smstart sm
      Form{0xd503'447fU, false, false}, // smstop za
      Form{0xd51b'4240U, false, false}, // msr svcr, x0
      Form{0xd53b'4240U, false, false}, // mrs x0, svcr
      Form{0xd53b'd0a1U, false, false}, // mrs x1, tpidr2_el0
      Form{0xd51b'd0a1U, false, false}, // msr tpidr2_el0, x1
      Form{0xe100'2043U, true, false},  // ldr za[w13, 3], [x2, #3, mul vl]
      Form{0xe120'03e0U, true, false},  // str za[w12, 0], [sp]
      Form{0xc008'00ffU, true, false},  // zero {za}
      Form{0xe11f'80a0U, true, true},   // ldr zt0, [x5]
      Form{0xe13f'83e0U, true, true},   // str zt0, [sp]
      Form{0xc048'0001U, true, true},   // zero {zt0}
  };
  for (const bool sme2 : {true, false}) {
    const auto host = sme_host(64, sme2);
    const uml::cpu::HostCapabilitiesScope scope{host};
    for (const auto &form : control)
      for (std::uint8_t mode = 0; mode <= arm64::svcr_mask; ++mode) {
        const bool expected = (form.needs_sme2 && !sme2) ||
                              (form.needs_za && (mode & arm64::svcr_za) == 0U);
        if (illegal_in(form.word, mode) != expected) {
          std::fprintf(stderr, "%08x mode %u sme2 %d\n", form.word, mode,
                       sme2);
          require(false);
        }
      }
    /* MSR SVCR immediates outside SM, ZA and SMZA stay unallocated. */
    for (const std::uint32_t unallocated :
         {0xd503'407fU, 0xd503'417fU, 0xd503'487fU, 0xd503'4f7fU})
      require(illegal_in(unallocated, 0));
  }
  const auto bare = sme_host(0);
  const uml::cpu::HostCapabilitiesScope scope{bare};
  const std::array unknown{0xffff'ffffU};
  const auto reference = jit_test::compile_block(0x4f30'0000U, unknown);
  for (const auto &form : control) {
    const std::array program{form.word};
    require(jit_test::compile_block(0x4f30'0000U, program).words ==
            reference.words);
  }
}

/* The prologue's enter names the host mode the translation runs in. */
void enter_names_the_host_mode() {
  constexpr std::array program{add_x0_one, system_call};
  for (const bool sme2 : {true, false}) {
    const auto host = sme_host(64, sme2);
    const uml::cpu::HostCapabilitiesScope scope{host};
    for (std::uint8_t mode = arm64::svcr_sm; mode <= arm64::svcr_mask;
         mode = static_cast<std::uint8_t>(mode + 2U)) {
      const auto code =
          jit_test::compile_block_in_mode(0x4f40'0000U, program, mode).code;
      const auto expected = arm64::jit::jit_sme_host_mode(mode, sme2);
      const auto first = code.words.begin();
      const auto last =
          first + static_cast<std::ptrdiff_t>(code.internal_entry_word);
      require(std::find(first, last,
                        0xd280'0009U | (std::uint32_t{expected} << 5)) !=
              last);
    }
  }
}

void population_count_needs_no_advanced_simd() {
  for (const std::uint64_t value :
       {0ULL, 1ULL, 0x8000'0000'0000'0000ULL, ~0ULL, 0x5555'5555'5555'5555ULL,
        0x0123'4567'89ab'cdefULL, 0xffff'ffff'0000'0001ULL,
        0x0000'0001'ffff'ffffULL}) {
    require(jit_test::gpr_population_count_for_testing(true, value) ==
            static_cast<std::uint64_t>(std::popcount(value)));
    require(jit_test::gpr_population_count_for_testing(false, value) ==
            static_cast<std::uint64_t>(
                std::popcount(static_cast<std::uint32_t>(value))));
  }
  std::uint64_t state = 0x243f'6a88'85a3'08d3ULL;
  for (int round = 0; round < 10'000; ++round) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    require(jit_test::gpr_population_count_for_testing(true, state) ==
            static_cast<std::uint64_t>(std::popcount(state)));
    require(jit_test::gpr_population_count_for_testing(false, state) ==
            static_cast<std::uint64_t>(
                std::popcount(static_cast<std::uint32_t>(state))));
  }
}

} // namespace

int main(int argc, char **argv) {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(argc == 2);
  streaming_translations_hold_no_fp_simd_word(argv[1]);
  translations_do_not_depend_on_the_svl(argv[1]);
  control_core_follows_profile_and_mode();
  enter_names_the_host_mode();
  if (arm64::native_jit_available())
    population_count_needs_no_advanced_simd();
  return 0;
}
