// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The JIT's non-streaming SVE translations, checked as code: the host
 * implements no SVE, and the guest's SVE and SME state outside streaming mode
 * lives in the AdvSIMD register file, so no SM = 0 translation of a guest
 * block may hold an SVE or SME word, whatever the block holds, whichever
 * profile the virtual CPU has, in either ZA state and every link shape.
 *
 * The profiles are the SVE profile alone, with SME, and with SME2, injected
 * into the capabilities the compiler translates for. Every row of the shared
 * encoding table the profile executes outside streaming mode (the SCLAMP
 * class included) is compiled from random words alone, behind an FP
 * instruction, behind PTRUE of P0-P7 (where the translation knows every
 * governing predicate all-active and emits no check) and in blocks that mix
 * words of several rows; every witness word of the A64 evidence table, which
 * the registration passes as the only argument, is compiled behind and ahead
 * of an SVE word. The check reads the executable words of each translation,
 * before its literal pool. A control compiles the same rows in streaming mode
 * on an SME host, where SVE words are emitted, so the check is shown to see
 * them.
 */
#include "runtime_test_route.hpp"
#include "require.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/detail/sve_semantics.hpp"
#include "cpu/arm64/jit/jit_semantic_sve.hpp"
#include "cpu/arm64/jit/jit_sve_support.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/arm64/sve_profile.hpp"
#include "cpu/host_capabilities.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace arm64 = uml::cpu::arm64;
namespace jit_test = uml::cpu::arm64::jit::testing;
using uml::cpu::ArmFeature;
using uml::cpu::HostCapabilities;
using uml::testing::require;

constexpr std::uint64_t pc = 0x4f40'0000U;
constexpr std::uint32_t system_call = 0xd400'0001U;
/* fadd s31, s31, s31: leaves the guest's FP environment active. */
constexpr std::uint32_t activate = 0x1e3f'2bffU;
/* ptrue p<n>.b, all. */
constexpr std::uint32_t ptrue_b = 0x2518'e3e0U;

/* The probed host with the SVE profile injected, with SME and SME2 on
 * request, plus the BF16 and I8MM tokens their SVE forms need; the compiler
 * only chooses between inline code and the library for them. */
[[nodiscard]] HostCapabilities host_with(bool sme, bool sme2) {
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
  if (sme) {
    for (const auto feature : {ArmFeature::sme, ArmFeature::sme_f64f64,
                               ArmFeature::sme_i16i64})
      host.supported.set(static_cast<std::size_t>(feature));
    host.supported.set(static_cast<std::size_t>(ArmFeature::sme2), sme2);
    host.sme_svl_bytes = 64;
  }
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

private:
  std::uint64_t state_;
};

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
    result.push_back(
        {column[0], static_cast<std::uint32_t>(
                        std::stoul(column[5], nullptr, 16))});
  }
  require(result.size() > 2000U);
  return result;
}

struct Checked final {
  std::size_t translations{};
  std::size_t executed_sve_words{};
  std::size_t library_blocks{};
  std::size_t inline_blocks{};
};

/* One program in both ZA states of SM = 0 and every link shape: the
 * executable words hold no SVE or SME word. */
void check_program(std::span<const std::uint32_t> program,
                   const std::string &label, Checked &checked) {
  for (const std::uint8_t mode : {std::uint8_t{0}, arm64::svcr_za})
    for (const auto shape :
         {jit_test::LinkShape::unlinked, jit_test::LinkShape::direct,
          jit_test::LinkShape::indirect, jit_test::LinkShape::tracked}) {
      const auto code =
          jit_test::compile_block_in_mode(pc, program, mode, shape).code;
      require(code.executable_words <= code.words.size());
      for (std::size_t index = 0; index < code.executable_words; ++index)
        if (arm64::detail::sme_encoding_space(code.words[index])) {
          std::fprintf(stderr,
                       "%s mode %u: SVE or SME word %08x at %zu of the "
                       "translation of",
                       label.c_str(), mode, code.words[index], index);
          for (const auto word : program)
            std::fprintf(stderr, " %08x", word);
          std::fprintf(stderr, "\n");
          require(false);
        }
      ++checked.translations;
      if (shape == jit_test::LinkShape::unlinked && mode == 0U &&
          !code.guest_illegal) {
        /* A word that leaves translated code for the library ends its
         * block there. */
        if (code.guest_instructions == program.size())
          ++checked.inline_blocks;
        else
          ++checked.library_blocks;
      }
    }
}

/* Words of `row` that decode to it on the scoped profile. */
[[nodiscard]] std::vector<std::uint32_t>
words_of(const arm64::detail::SmeEncoding &row, Random &random,
         arm64::detail::SmeFeatureMask features, std::size_t count) {
  std::vector<std::uint32_t> words;
  for (std::size_t attempt = 0; attempt < 4U * count && words.size() < count;
       ++attempt) {
    const auto word = row.value | (static_cast<std::uint32_t>(random.next()) &
                                   ~row.mask);
    if (arm64::detail::decode_sve_non_streaming(word, features) != &row ||
        arm64::jit::sve_word_reserved(row, word))
      continue;
    words.push_back(word);
  }
  return words;
}

/* Every row the profile executes, from random words, alone and behind each
 * kind of prefix, and mixed. */
void rows_emit_no_sve_word(const HostCapabilities &host, const char *name,
                           std::size_t per_row, Checked &checked) {
  const uml::cpu::HostCapabilitiesScope scope{host};
  const auto features = arm64::implemented_sve_features(host);
  Random random{0x5ec0'de01U};
  std::vector<std::uint32_t> pool;
  std::size_t rows = 0;
  for (const auto &row : arm64::detail::sme_encodings) {
    if ((row.non_streaming_requirement & arm64::detail::sme_feature_sve) ==
            0U ||
        (row.non_streaming_requirement & features) !=
            row.non_streaming_requirement ||
        (row.mode & arm64::detail::sme_mode_za) != 0U)
      continue;
    ++rows;
    for (const auto word : words_of(row, random, features, per_row)) {
      const std::array alone{word, system_call};
      const std::array behind_fp{activate, word, system_call};
      const std::array behind_ptrue{
          ptrue_b,      ptrue_b | 1U, ptrue_b | 2U, ptrue_b | 3U,
          ptrue_b | 4U, ptrue_b | 5U, ptrue_b | 6U, ptrue_b | 7U,
          word,         system_call};
      check_program(alone, name, checked);
      check_program(behind_fp, name, checked);
      check_program(behind_ptrue, name, checked);
      pool.push_back(word);
    }
  }
  /* Blocks of several rows' words, as real code runs them. */
  for (std::size_t block = 0; block < 4000U && pool.size() > 8U; ++block) {
    std::vector<std::uint32_t> program;
    if (block % 3U == 0U)
      program.push_back(ptrue_b);
    const auto length = 2U + random.next() % 6U;
    for (std::size_t n = 0; n < length; ++n)
      program.push_back(pool[random.next() % pool.size()]);
    program.push_back(system_call);
    check_program(program, name, checked);
  }
  std::fprintf(stderr, "%s: %zu rows\n", name, rows);
  require(rows > 400U);
}

/* The witness words of the evidence table around an SVE word, the cases the
 * hot-set lowerings and the live marker meet next to base and AdvSIMD code. */
void witnesses_emit_no_sve_word(const HostCapabilities &host,
                                const char *bindings, Checked &checked) {
  const uml::cpu::HostCapabilitiesScope scope{host};
  constexpr std::uint32_t sve_before = 0x0420'e020U; // cntb x0
  constexpr std::uint32_t sve_after = 0x04a0'0020U;  // add z0.s, z1.s, z0.s
  for (const auto &witness : witnesses(bindings)) {
    const std::array alone{witness.word, system_call};
    const std::array after{sve_before, witness.word, sve_after, system_call};
    const std::array before_ptrue{ptrue_b, witness.word, system_call};
    check_program(alone, witness.id, checked);
    check_program(after, witness.id, checked);
    check_program(before_ptrue, witness.id, checked);
  }
}

/* The check can fail: a streaming translation of an SVE word on an SME host
 * holds SVE words, since the host executes them there. */
void streaming_translations_hold_them(Checked &checked) {
  auto host = host_with(true, false);
  const uml::cpu::HostCapabilitiesScope scope{host};
  constexpr std::array program{0x0420'e020U, // cntb x0
                               0x04a0'0020U, // add z0.s, z1.s, z0.s
                               system_call};
  const auto code =
      jit_test::compile_block_in_mode(pc, program, arm64::svcr_sm).code;
  std::size_t found = 0;
  for (std::size_t index = 0; index < code.executable_words; ++index)
    found += arm64::detail::sme_encoding_space(code.words[index]) ? 1U : 0U;
  checked.executed_sve_words = found;
  require(found != 0U);
}

} // namespace

int main(int argc, char **argv) {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(argc == 2);
  require(arm64::jit::sve_support == arm64::SveSupport::complete);
  Checked checked;
  rows_emit_no_sve_word(host_with(false, false), "SVE", 8U, checked);
  rows_emit_no_sve_word(host_with(true, false), "SVE+SME", 6U, checked);
  rows_emit_no_sve_word(host_with(true, true), "SVE+SME+SME2", 6U, checked);
  witnesses_emit_no_sve_word(host_with(true, true), argv[1], checked);
  streaming_translations_hold_them(checked);
  std::fprintf(stderr,
               "checked %zu SM = 0 translations, %zu blocks inline and %zu "
               "with a library word; a streaming translation holds %zu SVE "
               "words\n",
               checked.translations, checked.inline_blocks,
               checked.library_blocks, checked.executed_sve_words);
  require(checked.translations > 100'000U && checked.inline_blocks != 0U &&
          checked.library_blocks != 0U);
  return 0;
}
