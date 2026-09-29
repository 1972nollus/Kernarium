// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_simd_fixed_transform_test_support.hpp"

#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <set>

namespace a64 = uml::testing::a64;

extern "C" {
#define UML_DECLARE_FIXED_GADGET(name)                                         \
  void uml_arm64_gadget_simd_##name();                                         \
  void uml_arm64_gadget_simd_##name##_soft();
UML_DECLARE_FIXED_GADGET(sha1c)
UML_DECLARE_FIXED_GADGET(sha1p)
UML_DECLARE_FIXED_GADGET(sha1m)
UML_DECLARE_FIXED_GADGET(sha1su0)
UML_DECLARE_FIXED_GADGET(sha256h)
UML_DECLARE_FIXED_GADGET(sha256h2)
UML_DECLARE_FIXED_GADGET(sha256su1)
UML_DECLARE_FIXED_GADGET(sha1h)
UML_DECLARE_FIXED_GADGET(sha1su1)
UML_DECLARE_FIXED_GADGET(sha256su0)
UML_DECLARE_FIXED_GADGET(sha512h)
UML_DECLARE_FIXED_GADGET(sha512h2)
UML_DECLARE_FIXED_GADGET(sha512su1)
UML_DECLARE_FIXED_GADGET(sha512su0)
UML_DECLARE_FIXED_GADGET(sm3partw1)
UML_DECLARE_FIXED_GADGET(sm3partw2)
UML_DECLARE_FIXED_GADGET(sm3ss1)
UML_DECLARE_FIXED_GADGET(sm4ekey)
UML_DECLARE_FIXED_GADGET(sm4e)
#undef UML_DECLARE_FIXED_GADGET

#define UML_DECLARE_INDEXED_FIXED_GADGET(name, index)                          \
  void uml_arm64_gadget_simd_##name##_##index();                               \
  void uml_arm64_gadget_simd_##name##_##index##_soft();
#define UML_DECLARE_INDEXED_FIXED_OPERATION(name)                              \
  UML_DECLARE_INDEXED_FIXED_GADGET(name, 0)                                    \
  UML_DECLARE_INDEXED_FIXED_GADGET(name, 1)                                    \
  UML_DECLARE_INDEXED_FIXED_GADGET(name, 2)                                    \
  UML_DECLARE_INDEXED_FIXED_GADGET(name, 3)
UML_DECLARE_INDEXED_FIXED_OPERATION(sm3tt1a)
UML_DECLARE_INDEXED_FIXED_OPERATION(sm3tt1b)
UML_DECLARE_INDEXED_FIXED_OPERATION(sm3tt2a)
UML_DECLARE_INDEXED_FIXED_OPERATION(sm3tt2b)
#undef UML_DECLARE_INDEXED_FIXED_OPERATION
#undef UML_DECLARE_INDEXED_FIXED_GADGET
}

namespace test = arm64_simd_fixed_transform_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using Gadget = void (*)();

constexpr std::array<Gadget, 7> native32_three{
    uml_arm64_gadget_simd_sha1c,     uml_arm64_gadget_simd_sha1p,
    uml_arm64_gadget_simd_sha1m,     uml_arm64_gadget_simd_sha1su0,
    uml_arm64_gadget_simd_sha256h,   uml_arm64_gadget_simd_sha256h2,
    uml_arm64_gadget_simd_sha256su1,
};
constexpr std::array<Gadget, 7> software32_three{
    uml_arm64_gadget_simd_sha1c_soft,     uml_arm64_gadget_simd_sha1p_soft,
    uml_arm64_gadget_simd_sha1m_soft,     uml_arm64_gadget_simd_sha1su0_soft,
    uml_arm64_gadget_simd_sha256h_soft,   uml_arm64_gadget_simd_sha256h2_soft,
    uml_arm64_gadget_simd_sha256su1_soft,
};
constexpr std::array<Gadget, 3> native32_two{
    uml_arm64_gadget_simd_sha1h,
    uml_arm64_gadget_simd_sha1su1,
    uml_arm64_gadget_simd_sha256su0,
};
constexpr std::array<Gadget, 3> software32_two{
    uml_arm64_gadget_simd_sha1h_soft,
    uml_arm64_gadget_simd_sha1su1_soft,
    uml_arm64_gadget_simd_sha256su0_soft,
};
constexpr std::array<Gadget, 3> native64_three{
    uml_arm64_gadget_simd_sha512h,
    uml_arm64_gadget_simd_sha512h2,
    uml_arm64_gadget_simd_sha512su1,
};
constexpr std::array<Gadget, 3> software64_three{
    uml_arm64_gadget_simd_sha512h_soft,
    uml_arm64_gadget_simd_sha512h2_soft,
    uml_arm64_gadget_simd_sha512su1_soft,
};
constexpr std::array<Gadget, 2> native_later_three{
    uml_arm64_gadget_simd_sm3partw1,
    uml_arm64_gadget_simd_sm3partw2,
};
constexpr std::array<Gadget, 2> software_later_three{
    uml_arm64_gadget_simd_sm3partw1_soft,
    uml_arm64_gadget_simd_sm3partw2_soft,
};

#define UML_INDEXED_FIXED_GADGETS(implementation)                              \
  std::array<Gadget, 16> {                                                     \
    uml_arm64_gadget_simd_sm3tt1a_0##implementation,                           \
        uml_arm64_gadget_simd_sm3tt1a_1##implementation,                       \
        uml_arm64_gadget_simd_sm3tt1a_2##implementation,                       \
        uml_arm64_gadget_simd_sm3tt1a_3##implementation,                       \
        uml_arm64_gadget_simd_sm3tt1b_0##implementation,                       \
        uml_arm64_gadget_simd_sm3tt1b_1##implementation,                       \
        uml_arm64_gadget_simd_sm3tt1b_2##implementation,                       \
        uml_arm64_gadget_simd_sm3tt1b_3##implementation,                       \
        uml_arm64_gadget_simd_sm3tt2a_0##implementation,                       \
        uml_arm64_gadget_simd_sm3tt2a_1##implementation,                       \
        uml_arm64_gadget_simd_sm3tt2a_2##implementation,                       \
        uml_arm64_gadget_simd_sm3tt2a_3##implementation,                       \
        uml_arm64_gadget_simd_sm3tt2b_0##implementation,                       \
        uml_arm64_gadget_simd_sm3tt2b_1##implementation,                       \
        uml_arm64_gadget_simd_sm3tt2b_2##implementation,                       \
        uml_arm64_gadget_simd_sm3tt2b_3##implementation                        \
  }
constexpr auto native_indexed = UML_INDEXED_FIXED_GADGETS();
constexpr auto software_indexed = UML_INDEXED_FIXED_GADGETS(_soft);
#undef UML_INDEXED_FIXED_GADGETS

[[nodiscard]] std::uintptr_t address(Gadget gadget) noexcept {
  return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] constexpr std::uintptr_t vector_offset(unsigned index) noexcept {
  return offsetof(test::arm64::CpuState, vectors) +
         index * sizeof(test::arm64::VectorRegister);
}

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction,
                                         bool force_software) {
  gadgets::BuildOptions options{};
  options.force_software_fixed_transforms = force_software;
  return gadgets::Builder{}.build(
      test::engine::engine_test_code_base, std::array{instruction}, {},
      std::pmr::get_default_resource(), {}, options);
}

template <std::size_t count>
void check_stream(std::uint32_t instruction, Gadget native, Gadget software,
                  bool host_native,
                  const std::array<unsigned, count> &registers,
                  std::set<std::uintptr_t> &selected,
                  std::set<std::uintptr_t> &portable) {
  std::array<std::uintptr_t, 4> expected_offsets{};
  for (unsigned index = 0; index < count; ++index)
    expected_offsets[index] = vector_offset(registers[index]);
  for (const bool forced : {false, true}) {
    const auto built = build(instruction, forced);
    const auto words = built.program.words();
    test::require(built.complete && words.size() == 4U,
                  "fixed transform stream shape is not exact");
    const auto expected_gadget = !forced && host_native ? native : software;
    test::require(words[0] == address(expected_gadget),
                  "fixed transform host path remained at runtime");
    test::require(words[1] == gadgets::selection::packed_u16_operands(
                                  expected_offsets[0], expected_offsets[1],
                                  expected_offsets[2], expected_offsets[3]),
                  "fixed transform offsets were not packed");
    (forced ? portable : selected).insert(words[0]);
  }
}

void translation_selects_every_operation_and_control(
    std::set<std::uintptr_t> &selected, std::set<std::uintptr_t> &portable) {
  constexpr test::Registers3 three{31, 30, 29};
  constexpr test::Registers2 two{31, 30};
  constexpr test::Registers4 four{31, 30, 29, 28};
  const bool first_native = test::engine::host_supports(test::feature1());
  const bool second_native = test::engine::host_supports(test::feature2());
  const bool wide_native = test::engine::host_supports(test::feature64());
  const bool later_three_native = test::engine::host_supports(test::feature3());
  const bool later_four_native = test::engine::host_supports(test::feature4());

  for (unsigned operation = 0; operation < 7U; ++operation)
    check_stream(test::transform32_three(operation, three),
                 native32_three[operation], software32_three[operation],
                 operation <= 3U ? first_native : second_native,
                 std::array{three.destination, three.first, three.second},
                 selected, portable);
  for (unsigned operation = 0; operation < 3U; ++operation)
    check_stream(test::transform32_two(operation, two), native32_two[operation],
                 software32_two[operation],
                 operation <= 1U ? first_native : second_native,
                 std::array{two.destination, two.source}, selected, portable);
  for (unsigned operation = 0; operation < 3U; ++operation)
    check_stream(test::transform64_three(operation, three),
                 native64_three[operation], software64_three[operation],
                 wide_native,
                 std::array{three.destination, three.first, three.second},
                 selected, portable);
  check_stream(test::transform64_two(two), uml_arm64_gadget_simd_sha512su0,
               uml_arm64_gadget_simd_sha512su0_soft, wide_native,
               std::array{two.destination, two.source}, selected, portable);

  for (unsigned operation = 0; operation < 2U; ++operation)
    check_stream(test::transform3_three(operation, three),
                 native_later_three[operation], software_later_three[operation],
                 later_three_native,
                 std::array{three.destination, three.first, three.second},
                 selected, portable);
  check_stream(
      test::transform3_four(four), uml_arm64_gadget_simd_sm3ss1,
      uml_arm64_gadget_simd_sm3ss1_soft, later_three_native,
      std::array{four.destination, four.first, four.second, four.third},
      selected, portable);
  for (unsigned operation = 0; operation < 4U; ++operation)
    for (unsigned lane = 0; lane < 4U; ++lane) {
      const auto slot = operation * 4U + lane;
      check_stream(test::transform3_indexed(operation, lane, three),
                   native_indexed[slot], software_indexed[slot],
                   later_three_native,
                   std::array{three.destination, three.first, three.second},
                   selected, portable);
    }
  check_stream(test::transform4_three(three), uml_arm64_gadget_simd_sm4ekey,
               uml_arm64_gadget_simd_sm4ekey_soft, later_four_native,
               std::array{three.destination, three.first, three.second},
               selected, portable);
  check_stream(test::transform4_two(two), uml_arm64_gadget_simd_sm4e,
               uml_arm64_gadget_simd_sm4e_soft, later_four_native,
               std::array{two.destination, two.source}, selected, portable);

  test::require(selected.size() == 35U,
                "fixed transform current-host selection is incomplete");
  test::require(portable.size() == 35U,
                "fixed transform portable selection is incomplete");
}

void baseline_regression_stream_consumes_every_operand() {
  constexpr std::uint32_t instruction = 0x5e02'0020U;
  const auto built = build(instruction, false);
  const auto words = built.program.words();
  test::require(
      built.complete && words.size() == 4U,
      "baseline regression stream has mismatched producer/consumer shape");
  test::require(words[1] == gadgets::selection::packed_u16_operands(
                                  vector_offset(0), vector_offset(1),
                                  vector_offset(2)),
                "baseline regression offsets were not packed");
}

struct GadgetShape final {
  unsigned instructions;
  unsigned conditional_branches;
};

[[nodiscard]] GadgetShape shape(Gadget gadget) {
  const auto body = a64::body_shape(reinterpret_cast<std::uintptr_t>(gadget),
                                    {.window = 192U});
  if (!body.reached_dispatch)
    test::fail("fixed transform gadget did not reach threaded dispatch");
  return {body.instructions + 1U, body.conditional_branches};
}

void every_definition_is_unique_branch_free_and_bounded() {
  std::set<std::uintptr_t> definitions;
  unsigned maximum_native = 0;
  unsigned maximum_helper_software = 0;
  unsigned maximum_vector_software = 0;
  const auto inspect = [&](Gadget gadget, unsigned &maximum) {
    const auto properties = shape(gadget);
    test::require(properties.conditional_branches == 0U,
                  "fixed transform gadget contains conditional branch");
    maximum = std::max(maximum, properties.instructions);
    definitions.insert(address(gadget));
  };
  for (unsigned operation = 0; operation < native32_three.size(); ++operation) {
    inspect(native32_three[operation], maximum_native);
    inspect(software32_three[operation], maximum_helper_software);
  }
  for (unsigned operation = 0; operation < native32_two.size(); ++operation) {
    inspect(native32_two[operation], maximum_native);
    inspect(software32_two[operation], maximum_helper_software);
  }
  for (unsigned operation = 0; operation < native64_three.size(); ++operation) {
    inspect(native64_three[operation], maximum_native);
    inspect(software64_three[operation], maximum_helper_software);
  }
  inspect(uml_arm64_gadget_simd_sha512su0, maximum_native);
  inspect(uml_arm64_gadget_simd_sha512su0_soft, maximum_helper_software);
  for (unsigned operation = 0; operation < native_later_three.size();
       ++operation) {
    inspect(native_later_three[operation], maximum_native);
    inspect(software_later_three[operation], maximum_vector_software);
  }
  inspect(uml_arm64_gadget_simd_sm3ss1, maximum_native);
  inspect(uml_arm64_gadget_simd_sm3ss1_soft, maximum_vector_software);
  for (unsigned slot = 0; slot < native_indexed.size(); ++slot) {
    inspect(native_indexed[slot], maximum_native);
    inspect(software_indexed[slot], maximum_vector_software);
  }
  inspect(uml_arm64_gadget_simd_sm4ekey, maximum_native);
  inspect(uml_arm64_gadget_simd_sm4ekey_soft, maximum_vector_software);
  inspect(uml_arm64_gadget_simd_sm4e, maximum_native);
  inspect(uml_arm64_gadget_simd_sm4e_soft, maximum_vector_software);

  test::require(
      definitions.size() == 70U,
      "fixed transform gadget definition is duplicate or unreachable");
  test::require(maximum_native <= 12U,
                "native fixed transform gadget exceeds instruction budget");
  test::require(maximum_helper_software <= 13U,
                "portable helper gadget exceeds instruction budget");
  test::require(maximum_vector_software <= 128U,
                "portable vector gadget exceeds instruction budget");
}

} // namespace

int main() {
  std::set<std::uintptr_t> selected;
  std::set<std::uintptr_t> portable;
  translation_selects_every_operation_and_control(selected, portable);
  baseline_regression_stream_consumes_every_operand();
  every_definition_is_unique_branch_free_and_bounded();
  return 0;
}
