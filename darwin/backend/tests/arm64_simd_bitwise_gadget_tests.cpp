// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_simd_bitwise_test_support.hpp"

#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" {
void uml_arm64_gadget_nop();
void uml_arm64_gadget_simd_and_8b();
void uml_arm64_gadget_simd_and_16b();
void uml_arm64_gadget_simd_bic_8b();
void uml_arm64_gadget_simd_bic_16b();
void uml_arm64_gadget_simd_orr_8b();
void uml_arm64_gadget_simd_orr_16b();
void uml_arm64_gadget_simd_orn_8b();
void uml_arm64_gadget_simd_orn_16b();
void uml_arm64_gadget_simd_eor_8b();
void uml_arm64_gadget_simd_eor_16b();
void uml_arm64_gadget_simd_bsl_8b();
void uml_arm64_gadget_simd_bsl_16b();
void uml_arm64_gadget_simd_bit_8b();
void uml_arm64_gadget_simd_bit_16b();
void uml_arm64_gadget_simd_bif_8b();
void uml_arm64_gadget_simd_bif_16b();
void uml_arm64_gadget_simd_mov_8b();
void uml_arm64_gadget_simd_mov_16b();
}

namespace bitwise = arm64_simd_bitwise_test;
namespace engine_tests = uml::backend::tests;
namespace gadgets = uml::backend::arm64::gadgets;

namespace {

using Gadget = void (*)();

constexpr std::array<std::array<Gadget, 2>, 8> expected_gadgets{{
    {uml_arm64_gadget_simd_and_8b, uml_arm64_gadget_simd_and_16b},
    {uml_arm64_gadget_simd_bic_8b, uml_arm64_gadget_simd_bic_16b},
    {uml_arm64_gadget_simd_orr_8b, uml_arm64_gadget_simd_orr_16b},
    {uml_arm64_gadget_simd_orn_8b, uml_arm64_gadget_simd_orn_16b},
    {uml_arm64_gadget_simd_eor_8b, uml_arm64_gadget_simd_eor_16b},
    {uml_arm64_gadget_simd_bsl_8b, uml_arm64_gadget_simd_bsl_16b},
    {uml_arm64_gadget_simd_bit_8b, uml_arm64_gadget_simd_bit_16b},
    {uml_arm64_gadget_simd_bif_8b, uml_arm64_gadget_simd_bif_16b},
}};

[[noreturn]] void fail(const char *message) {
  std::fprintf(stderr, "%s\n", message);
  std::abort();
}

void require(bool condition, const char *message) {
  if (!condition)
    fail(message);
}

[[nodiscard]] std::uintptr_t address_of(Gadget gadget) noexcept {
  return reinterpret_cast<std::uintptr_t>(gadget);
}

[[nodiscard]] constexpr std::uintptr_t vector_offset(unsigned index) noexcept {
  return offsetof(uml::backend::arm64::CpuState, vectors) +
         index * sizeof(uml::backend::arm64::VectorRegister);
}

void encoding_examples() {
  require(bitwise::instruction(true, bitwise::Operation::bit_and, {0, 1, 2}) ==
              0x4e22'1c20U,
          "AND vector encoding mismatch");
  require(
      bitwise::instruction(false, bitwise::Operation::bit_clear, {3, 4, 5}) ==
          0x0e65'1c83U,
      "BIC vector encoding mismatch");
  require(bitwise::instruction(true, bitwise::Operation::insert_true,
                               {18, 19, 20}) == 0x6eb4'1e72U,
          "BIT vector encoding mismatch");
}

void production_specialization() {
  constexpr auto base = engine_tests::engine_test_code_base;
  constexpr bitwise::Registers registers{7, 11, 19};
  for (unsigned operation = 0; operation < 8; ++operation)
    for (const bool q : {false, true}) {
      const auto built = gadgets::Builder{}.build(
          base, std::array{bitwise::instruction(
                    q, static_cast<bitwise::Operation>(operation), registers)});
      require(built.complete && built.program.words().size() == 6U,
              "ordinary bitwise instruction has an unexpected stream shape");
      require(built.program.words()[0] ==
                  address_of(expected_gadgets[operation][q ? 1U : 0U]),
              "operation or arrangement was not specialized in translation");
      require(built.program.words()[1] == vector_offset(registers.destination) &&
                  built.program.words()[2] == vector_offset(registers.left) &&
                  built.program.words()[3] == vector_offset(registers.right),
              "register fields were not translated to direct CpuState offsets");
    }

  for (const bool q : {false, true}) {
    const auto moved = gadgets::Builder{}.build(
        base, std::array{bitwise::instruction(
                  q, bitwise::Operation::bit_or, {5, 9, 9})});
    const auto expected =
        q ? uml_arm64_gadget_simd_mov_16b : uml_arm64_gadget_simd_mov_8b;
    require(moved.complete && moved.program.words().size() == 5U,
            "MOV alias has an unexpected stream shape");
    require(moved.program.words()[0] == address_of(expected),
            "MOV alias retained the generic two-source operation");
    require(moved.program.words()[1] == vector_offset(5) &&
                moved.program.words()[2] == vector_offset(9),
            "MOV alias retained encoded register indices");
  }

  const auto ordinary_nop =
      gadgets::Builder{}.build(base, std::array{0xd503'201fU});
  const auto self_move = gadgets::Builder{}.build(
      base, std::array{bitwise::instruction(
                true, bitwise::Operation::bit_or, {9, 9, 9})});
  require(ordinary_nop.complete && self_move.complete &&
              ordinary_nop.program.words().front() ==
                  address_of(uml_arm64_gadget_nop) &&
              self_move.program.words().size() == 3U &&
              self_move.program.words().front() ==
                  ordinary_nop.program.words().front(),
          "full-width self MOV was not folded to NOP");

  const auto narrow_self_move = gadgets::Builder{}.build(
      base, std::array{bitwise::instruction(
                false, bitwise::Operation::bit_or, {9, 9, 9})});
  require(narrow_self_move.complete &&
              narrow_self_move.program.words().size() == 5U &&
              narrow_self_move.program.words().front() ==
                  address_of(uml_arm64_gadget_simd_mov_8b) &&
              narrow_self_move.program.words()[1] == vector_offset(9) &&
              narrow_self_move.program.words()[2] == vector_offset(9),
          "narrow self MOV lost its required upper-half clear");
}

} // namespace

int main() {
  encoding_examples();
  production_specialization();
  return 0;
}
