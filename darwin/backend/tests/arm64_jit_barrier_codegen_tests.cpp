// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/host_features.hpp"
#include "runtime/jit/jit_test_api.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

namespace {

namespace jit_test = uml::backend::jit::testing;

constexpr std::uint32_t dsb_base = 0xd503'309fU;
constexpr std::uint32_t dmb_base = 0xd503'30bfU;
constexpr std::uint32_t isb_base = 0xd503'30dfU;
constexpr std::uint32_t dsb_nxs_base = 0xd503'323fU;
constexpr std::uint32_t sb = 0xd503'30ffU;
constexpr std::uint32_t terminal_system_call = 0xd400'0001U;

[[noreturn]] void fail(const char *message) {
  std::fprintf(stderr, "%s\n", message);
  std::abort();
}

void require(bool condition, const char *message) {
  if (!condition)
    fail(message);
}

[[nodiscard]] constexpr std::uint32_t standard(std::uint32_t base,
                                               unsigned option) noexcept {
  return base | ((option & 15U) << 8U);
}

[[nodiscard]] constexpr std::uint32_t nxs(unsigned scope) noexcept {
  return dsb_nxs_base | ((scope & 3U) << 10U);
}

template <std::size_t Size>
void require_words(const std::vector<std::uint32_t> &actual,
                   const std::array<std::uint32_t, Size> &expected,
                   const char *message) {
  require(actual.size() == expected.size() &&
              std::equal(actual.begin(), actual.end(), expected.begin()),
          message);
}

template <std::size_t Size>
[[nodiscard]] bool contains(std::span<const std::uint32_t> words,
                            const std::array<std::uint32_t, Size> &sequence) {
  return std::search(words.begin(), words.end(), sequence.begin(),
                     sequence.end()) != words.end();
}

void forced_feature_selection() {
  for (const auto base : {dmb_base, dsb_base, isb_base})
    for (unsigned option = 0; option < 16U; ++option) {
      const auto instruction = standard(base, option);
      require_words(
          jit_test::lower_memory_barrier_for_testing(instruction, false, false),
          std::array{instruction},
          "baseline barrier changed under the forced feature-off selector");
    }

  require_words(jit_test::lower_memory_barrier_for_testing(sb, true, false),
                std::array{sb},
                "native speculation barrier was not emitted one-for-one");
  require_words(jit_test::lower_memory_barrier_for_testing(sb, false, true),
                std::array{standard(dsb_base, 15U), standard(isb_base, 15U)},
                "portable speculation barrier substitution is incorrect");

  constexpr std::array fallback_options{3U, 7U, 11U, 15U};
  for (unsigned scope = 0; scope < 4U; ++scope) {
    const auto instruction = nxs(scope);
    require_words(
        jit_test::lower_memory_barrier_for_testing(instruction, false, true),
        std::array{instruction},
        "native restricted-completion barrier was not emitted one-for-one");
    require_words(
        jit_test::lower_memory_barrier_for_testing(instruction, true, false),
        std::array{standard(dsb_base, fallback_options[scope])},
        "portable restricted-completion barrier substitution is incorrect");
  }
}

void current_host_compiler_selection() {
  constexpr std::uint64_t pc = 0x4f80'0000;
  const std::array guest{
      sb, nxs(0), nxs(1), nxs(2), nxs(3), terminal_system_call,
  };
  const auto compiled = jit_test::compile_block(pc, guest);
  require(!compiled.guest_illegal,
          "allocated barrier sequence compiled as guest illegal");
  require(compiled.guest_instructions == guest.size(),
          "barrier sequence did not remain in one translated block");
  require(compiled.hot_words <= compiled.words.size(),
          "barrier compiler returned an invalid hot-code extent");
  const std::span<const std::uint32_t> hot{compiled.words.data(),
                                           compiled.hot_words};

  const auto &features = uml::backend::host_arm_features();
  const auto selected_sb = jit_test::lower_memory_barrier_for_testing(
      sb, features.has(uml::backend::ArmFeature::sb),
      features.has(uml::backend::ArmFeature::xs));
  require(std::search(hot.begin(), hot.end(), selected_sb.begin(),
                      selected_sb.end()) != hot.end(),
          "current-host speculation barrier lowering is absent from hot code");
  if (!features.has(uml::backend::ArmFeature::sb))
    require(std::find(hot.begin(), hot.end(), sb) == hot.end(),
            "unsupported speculation barrier remained in hot code");

  constexpr std::array fallback_options{3U, 7U, 11U, 15U};
  for (unsigned scope = 0; scope < 4U; ++scope) {
    const auto instruction = nxs(scope);
    const auto expected = features.has(uml::backend::ArmFeature::xs)
                              ? instruction
                              : standard(dsb_base, fallback_options[scope]);
    require(contains(hot, std::array{expected}),
            "current-host restricted-completion lowering is absent");
    if (!features.has(uml::backend::ArmFeature::xs))
      require(std::find(hot.begin(), hot.end(), instruction) == hot.end(),
              "unsupported restricted-completion word remained in hot code");
  }
}

} // namespace

int main() {
  forced_feature_selection();
  current_host_compiler_selection();
  std::puts("arm64-jit-barrier-codegen: translation selection passed");
  return 0;
}
