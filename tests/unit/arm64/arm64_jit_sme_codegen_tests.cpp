// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Streaming JIT translations, checked as code on injected SME profiles: no
 * AdvSIMD or other FP&SIMD word reaches an SM=1 translation, the host enters
 * streaming mode only in the external prologue and leaves it before every
 * host call and return, and only SM=1 translations touch the host's
 * streaming state. Every witness word of the A64 evidence table is compiled
 * in every mode and link shape.
 */
#include "runtime_test_route.hpp"
#include "require.hpp"
#include "cpu/arm64/jit/jit_emitter.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_semantic_memory.hpp"
#include "cpu/arm64/jit/jit_sme_transition.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/host_capabilities.hpp"

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

#ifndef UML_A64_EVIDENCE_BINDINGS
#error "UML_A64_EVIDENCE_BINDINGS names the A64 evidence table"
#endif

namespace {

namespace arm64 = uml::cpu::arm64;
namespace jit_test = uml::cpu::arm64::jit::testing;
using uml::testing::require;

constexpr std::uint32_t system_call = 0xd400'0001U;
constexpr std::uint32_t add_x0_one = 0x9100'0400U;
constexpr std::uint32_t blr_x16 = 0xd63f'0200U;
constexpr std::uint32_t ret = 0xd65f'03c0U;

struct Witness final {
  std::string id;
  std::uint32_t word{};
};

[[nodiscard]] std::vector<Witness> witnesses() {
  std::ifstream table{UML_A64_EVIDENCE_BINDINGS};
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

/* The host's own profile with the SME2 profile at `svl`, and without
 * FEAT_CSSC, so CNT takes its fallback as on the M4. */
[[nodiscard]] uml::cpu::HostCapabilities sme_host(unsigned svl) {
  auto host = uml::cpu::host_capabilities();
  for (const auto feature :
       {uml::cpu::ArmFeature::sme, uml::cpu::ArmFeature::sme2,
        uml::cpu::ArmFeature::sme_f64f64, uml::cpu::ArmFeature::sme_i16i64})
    host.supported.set(static_cast<std::size_t>(feature));
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
  std::size_t enters = 0;
  for (std::size_t index = 0; index < code.executable_words; ++index) {
    const auto word = code.words[index];
    if (streaming && fp_simd_word(word)) {
      std::fprintf(stderr, "%s mode %u: FP&SIMD word %08x at %zu\n",
                   label.c_str(), mode, word, index);
      require(false);
    }
    if (word == ret && streaming) {
      /* Every epilogue leaves streaming mode before restoring the frame. */
      require(index >= 6U && code.words[index - 5U] == blr_x16);
      require(callee(code, index - 5U) == Callee::leave);
    }
    if (word != blr_x16)
      continue;
    const auto target = callee(code, index);
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
      break;
    }
  }
  require(enters == (streaming ? 1U : 0U));
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

void streaming_translations_hold_no_fp_simd_word() {
  const auto host = sme_host(64);
  const uml::cpu::HostCapabilitiesScope scope{host};
  constexpr std::uint64_t pc = 0x4f00'0000U;
  Checked checked;
  for (const auto &witness : witnesses()) {
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
  };
  for (const auto &sequence : sequences)
    check_program(pc, sequence, "sequence", checked);
  std::fprintf(stderr, "checked %zu translations, %zu helper calls\n",
               checked.translations, checked.helper_calls);
  require(checked.translations > 70'000U);
  require(checked.helper_calls != 0U);
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

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  streaming_translations_hold_no_fp_simd_word();
  if (arm64::native_jit_available())
    population_count_needs_no_advanced_simd();
  return 0;
}
