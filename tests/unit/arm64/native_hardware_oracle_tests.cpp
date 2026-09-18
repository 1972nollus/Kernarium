// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_hardware_oracle.hpp"
#include "native_hardware_oracle_evidence.hpp"
#include "native_hardware_oracle_selection.hpp"
#include "instruction_evidence_digest.hpp"
#include "platform/apple/host_features.hpp"
#include "scratch_directory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <source_location>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace oracle = ::uml::emulator::tests::native_oracle;
namespace arm64 = ::uml::cpu::arm64;

namespace {

constexpr std::uint64_t guest_pc = 0x4f00'0000ULL;
constexpr std::uint64_t guest_data = 0x5100'0000ULL;

volatile sig_atomic_t parent_signal_count;

void parent_signal_handler(int) noexcept { parent_signal_count = 1; }

[[noreturn]] void fail(const char *message) {
  std::fprintf(stderr, "native hardware oracle: %s\n", message);
  std::exit(EXIT_FAILURE);
}

void require(bool condition, const char *message) {
  if (!condition)
    fail(message);
}

void allocation_identity_and_constraint_contract() {
  const oracle::AllocationIdentity identity{
      .stable_id = "BASE-0001",
      .architecture_file = "abs.xml",
      .architecture_encoding = "ABS_64_dp_1src",
      .architecture_mask = "fffffc00",
      .architecture_value = "dac02000",
      .architecture_constraints = "none",
      .decode_class = "dp_1src",
      .decode_encoding = "ABS_64_dp_1src",
      .decode_mask = "fffffc00",
      .decode_value = "dac02000",
      .decode_constraints = "none",
  };
  require(oracle::allocation_status_id(identity) ==
              "behavior-status-20d6846b709697162a3f",
          "allocation status identity differs from the official model");
  auto swapped = identity;
  swapped.architecture_encoding = identity.decode_class;
  swapped.decode_class = identity.architecture_encoding;
  require(oracle::allocation_status_id(swapped) !=
              oracle::allocation_status_id(identity),
          "swapped allocation identity fields were accepted");

  require(oracle::allocation_encoding_matches(
              0x5ac0'2000U, 0xffff'fc00U, 0x5ac0'2000U, "none",
              0xffff'fc00U, 0x5ac0'2000U, "Rd@0:5!=11111"),
          "valid constrained allocation did not match");
  require(!oracle::allocation_encoding_matches(
              0x5ac0'201fU, 0xffff'fc00U, 0x5ac0'2000U, "none",
              0xffff'fc00U, 0x5ac0'2000U, "Rd@0:5!=11111"),
          "reserved destination passed the allocation constraint");
  require(!oracle::allocation_encoding_matches(
              0x5ac0'2000U, 0xffff'fc00U, 0xdac0'2000U, "none",
              0xffff'fc00U, 0x5ac0'2000U, "none"),
          "allocation with a removed width bit matched");

  oracle::AllocationMaterialization materialization{
      .aggregate_id = "aggregate-00000000000000000000",
      .decode_partition_id = "decode-00000000000000000000",
      .allocation_status_id = "behavior-status-20d6846b709697162a3f",
      .declared_digest = std::string(64U, '1'),
      .observed_digest = std::string(64U, '2'),
      .architecture_features = "baseline",
      .producer = "arm64-base-allocation",
      .source_site = "0123456789abcdef0123",
      .source_file = "tests/unit/arm64/allocation.cpp",
      .source_line = 41U,
      .scope = oracle::SelectionScope::instruction,
      .guest_page_size = 16384U,
  };
  require(oracle::materialization_record_is_well_formed(materialization),
          "complete owning materialization record was rejected");

  auto malformed = materialization;
  malformed.aggregate_id = malformed.decode_partition_id;
  require(!oracle::materialization_record_is_well_formed(malformed),
          "swapped materialization identity fields were accepted");
  malformed = materialization;
  malformed.declared_digest[0] = 'A';
  require(!oracle::materialization_record_is_well_formed(malformed),
          "noncanonical materialization digest was accepted");
  malformed = materialization;
  malformed.source_line = 0U;
  require(!oracle::materialization_record_is_well_formed(malformed),
          "materialization without a source line was accepted");
  malformed = materialization;
  malformed.scope = static_cast<oracle::SelectionScope>(255U);
  require(!oracle::materialization_record_is_well_formed(malformed),
          "materialization with an invalid scope was accepted");
  malformed = materialization;
  malformed.guest_page_size = 8192U;
  require(!oracle::materialization_record_is_well_formed(malformed),
          "materialization with an invalid page mode was accepted");
}

[[nodiscard]] bool same_materialization(
    const oracle::AllocationMaterialization &left,
    const oracle::AllocationMaterialization &right) noexcept {
  return left.aggregate_id == right.aggregate_id &&
      left.decode_partition_id == right.decode_partition_id &&
      left.allocation_status_id == right.allocation_status_id &&
      left.declared_digest == right.declared_digest &&
      left.observed_digest == right.observed_digest &&
      left.architecture_features == right.architecture_features &&
      left.producer == right.producer &&
      left.source_site == right.source_site &&
      left.source_file == right.source_file &&
      left.source_line == right.source_line && left.scope == right.scope &&
      left.guest_page_size == right.guest_page_size;
}

[[noreturn]] void run_disabled_materialization_lookup_child() {
  if (unsetenv("UML_NATIVE_HARDWARE_EVIDENCE") != 0 ||
      setenv("UML_INSTRUCTION_EVIDENCE", "/path/that/must/not/exist.tsv",
             1) != 0 ||
      setenv("UML_INSTRUCTION_EVIDENCE_TEST", "invalid-test", 1) != 0 ||
      setenv("UML_INSTRUCTION_EVIDENCE_BINDINGS",
             "/path/that/must/not/exist/bindings.tsv", 1) != 0)
    _exit(2);

#line 284 "tests/unit/arm64/arm64_base_add_sub_test_support.hpp"
  const auto location = std::source_location::current();
#line 145 "tests/unit/arm64/native_hardware_oracle_tests.cpp"
  const auto materializations = oracle::lookup_native_materializations(
      0x9100'14a0U, oracle::SelectionScope::instruction, location);
  const auto representatives = oracle::select_native_allocations(
      0x9100'14a0U, oracle::SelectionScope::instruction, location);
  _exit(materializations.empty() &&
                representatives.allocation_status_ids.empty()
            ? 0
            : 3);
}

void ordinary_lookup_does_not_load_catalogs() {
  const auto child = fork();
  require(child >= 0, "cannot fork disabled materialization lookup contract");
  if (child == 0)
    run_disabled_materialization_lookup_child();
  int status{};
  require(waitpid(child, &status, 0) == child,
          "cannot wait for disabled materialization lookup contract");
  require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "ordinary lookup loaded native evidence catalogs");
}

[[noreturn]] void run_materialization_lookup_child(
    const std::string &bindings, const std::string &evidence) {
  if (setenv("UML_NATIVE_HARDWARE_EVIDENCE", "1", 1) != 0 ||
      setenv("UML_INSTRUCTION_EVIDENCE", evidence.c_str(), 1) != 0 ||
      setenv("UML_INSTRUCTION_EVIDENCE_TEST",
             "arm64-base-add-sub-immediate", 1) != 0 ||
      setenv("UML_INSTRUCTION_EVIDENCE_BINDINGS", bindings.c_str(), 1) != 0)
    _exit(2);

#line 284 "tests/unit/arm64/arm64_base_add_sub_test_support.hpp"
  const auto reviewed_location = std::source_location::current();
#line 285 "tests/unit/arm64/arm64_base_add_sub_test_support.hpp"
  const auto drifted_location = std::source_location::current();
#line 293 "tests/unit/arm64/arm64_base_add_sub_test_support.hpp"
  const auto distant_location = std::source_location::current();
#line 284 "tests/unit/arm64/unrelated_test_support.hpp"
  const auto unrelated_location = std::source_location::current();
#line 190 "tests/unit/arm64/native_hardware_oracle_tests.cpp"
  constexpr std::uint32_t instruction = 0x9100'14a0U;
  const auto first = oracle::lookup_native_materializations(
      instruction, oracle::SelectionScope::instruction, reviewed_location);
  const auto second = oracle::lookup_native_materializations(
      instruction, oracle::SelectionScope::instruction, reviewed_location);
  const auto drifted = oracle::lookup_native_materializations(
      instruction, oracle::SelectionScope::instruction, drifted_location);
  const auto distant = oracle::lookup_native_materializations(
      instruction, oracle::SelectionScope::instruction, distant_location);
  const auto unrelated = oracle::lookup_native_materializations(
      instruction, oracle::SelectionScope::instruction, unrelated_location);
  const auto primary_and_alias = oracle::lookup_native_materializations(
      0x9100'0020U, oracle::SelectionScope::instruction, reviewed_location);
  const auto wrong_scope = oracle::lookup_native_materializations(
      instruction, oracle::SelectionScope::program, reviewed_location);
  const auto wrong_word = oracle::lookup_native_materializations(
      0xd503'201fU, oracle::SelectionScope::instruction, reviewed_location);

#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
  const oracle::AllocationMaterialization expected{
      .aggregate_id = "aggregate-e322475d062e3174e45f",
      .decode_partition_id = "decode-8aa1c640163d86bae6ce",
      .allocation_status_id = "behavior-status-81e2702f0dc2afc54147",
      .declared_digest =
          "8b651c7214e31f07f8e251b87ff5bda75a69c786f648f9d4d7919db428c9f16c",
      .observed_digest =
          "fa31bc044f4618efcc8063d5c2be907206f17e627ef27c77baf911eea9397507",
      .architecture_features = "baseline",
      .producer = "arm64-base-add-sub-immediate",
      .source_site = "49c6fb4370024418e07b",
      .source_file =
          "tests/unit/arm64/arm64_base_add_sub_test_support.hpp",
      .source_line = 284U,
      .scope = oracle::SelectionScope::instruction,
      .guest_page_size = 16384U,
  };
  const oracle::AllocationMaterialization alias_expected{
      .aggregate_id = "aggregate-90a8918c96aedf2ade72",
      .decode_partition_id = "decode-8aa1c640163d86bae6ce",
      .allocation_status_id = "behavior-status-3b025e9ecc4e0509ca2f",
      .declared_digest =
          "83224fb6ae45fd8cf16497c92ea08fd28187bf126d7015522d06a51bd217da1f",
      .observed_digest =
          "33225c243b189b4e125875724557aa65912a81fa1e9dba1faef6c9835e5d2a42",
      .architecture_features = "baseline",
      .producer = "arm64-base-add-sub-immediate",
      .source_site = "49c6fb4370024418e07b",
      .source_file =
          "tests/unit/arm64/arm64_base_add_sub_test_support.hpp",
      .source_line = 284U,
      .scope = oracle::SelectionScope::instruction,
      .guest_page_size = 16384U,
  };
#elif DARWIN_UML_GUEST_PAGE_SIZE == 4096
  const oracle::AllocationMaterialization expected{
      .aggregate_id = "aggregate-1f98bad7f0a4bacfd88a",
      .decode_partition_id = "decode-8aa1c640163d86bae6ce",
      .allocation_status_id = "behavior-status-81e2702f0dc2afc54147",
      .declared_digest =
          "609b931900a43783cd760b2b1d570ba73ac57c09f512bb2ec516365b626760f5",
      .observed_digest =
          "fa31bc044f4618efcc8063d5c2be907206f17e627ef27c77baf911eea9397507",
      .architecture_features = "baseline",
      .producer = "arm64-base-add-sub-immediate",
      .source_site = "49c6fb4370024418e07b",
      .source_file =
          "tests/unit/arm64/arm64_base_add_sub_test_support.hpp",
      .source_line = 284U,
      .scope = oracle::SelectionScope::instruction,
      .guest_page_size = 4096U,
  };
  const oracle::AllocationMaterialization alias_expected{
      .aggregate_id = "aggregate-e04585b0656281dbbe4a",
      .decode_partition_id = "decode-8aa1c640163d86bae6ce",
      .allocation_status_id = "behavior-status-3b025e9ecc4e0509ca2f",
      .declared_digest =
          "9e9272ec27f2ea9c18d6a87ae6e1003b8a78801d20141c3199c4fc09c70656a8",
      .observed_digest =
          "33225c243b189b4e125875724557aa65912a81fa1e9dba1faef6c9835e5d2a42",
      .architecture_features = "baseline",
      .producer = "arm64-base-add-sub-immediate",
      .source_site = "49c6fb4370024418e07b",
      .source_file =
          "tests/unit/arm64/arm64_base_add_sub_test_support.hpp",
      .source_line = 284U,
      .scope = oracle::SelectionScope::instruction,
      .guest_page_size = 4096U,
  };
#else
#error Unsupported DARWIN_UML_GUEST_PAGE_SIZE
#endif

  const auto contains = [&](const oracle::AllocationMaterialization &item) {
    for (const auto &candidate : primary_and_alias)
      if (same_materialization(candidate, item))
        return true;
    return false;
  };
  const bool valid = first.size() == 1U && second.size() == 1U &&
      drifted.size() == 1U && distant.empty() && unrelated.empty() &&
      wrong_scope.empty() && wrong_word.empty() &&
      same_materialization(first.front(), expected) &&
      same_materialization(second.front(), expected) &&
      same_materialization(drifted.front(), expected) &&
      primary_and_alias.size() == 2U && contains(expected) &&
      contains(alias_expected);
  /* lookup() deliberately leaves the representative selector unconsumed.
   * _exit avoids running that selector's process-final completeness check. */
  _exit(valid ? 0 : 3);
}

void materialization_lookup_contract() {
  /* uml_add_unit_test() names the reviewed catalogs. The child's evidence
   * base stays in this test's scratch directory and is never written. */
  const auto *configured_bindings =
      std::getenv("UML_INSTRUCTION_EVIDENCE_BINDINGS");
  require(configured_bindings != nullptr && configured_bindings[0] != '\0',
          "UML_INSTRUCTION_EVIDENCE_BINDINGS is unset");
  const std::string bindings{configured_bindings};
  const auto evidence =
      (::uml::testing::scratch_directory() / "native-oracle-contract.tsv")
          .string();
  const auto child = fork();
  require(child >= 0, "cannot fork materialization lookup contract");
  if (child == 0)
    run_materialization_lookup_child(bindings, evidence);
  int status{};
  require(waitpid(child, &status, 0) == child,
          "cannot wait for materialization lookup contract");
  require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "materialization lookup did not return its exact owning record");
}

[[nodiscard]] oracle::Request baseline_request(std::uint32_t instruction,
                                               const arm64::CpuState &initial) {
  return {
      .instruction = instruction,
      .guest_pc = guest_pc,
      .initial = initial,
  };
}

void require_completed(const oracle::Observation &observation) {
  require(observation.eligibility == oracle::Eligibility::eligible,
          "eligible execution was reclassified");
  require(observation.status == oracle::ExecutionStatus::executed,
          "eligible instruction did not execute");
  require(observation.event == oracle::Event::completed,
          "instruction did not reach the trapping sentinel");
  require(observation.retired_instructions == 1,
          "completed instruction did not retire exactly once");
  require(observation.state.pc == guest_pc + sizeof(std::uint32_t),
          "completed PC was not normalized to the guest address");
}

void exact_arithmetic_specimen(const ::uml::cpu::HostCapabilities &host) {
  arm64::CpuState initial{};
  initial.registers[5] = 0x25;
  initial.nzcv = 0xa000'0000U;
  initial.pc = guest_pc;
  auto request = baseline_request(0x9100'14a0U, initial);
  request.oracle_case_id = "BASE-0005/ADD_64_addsub_imm";
  const auto observation = oracle::execute(request, host);
  oracle::record_observation(request.oracle_case_id,
                             "ADD immediate native specimen width=64 sh=0 "
                             "imm=5 rn=5 rd=0",
                             request.instruction, request.required_features,
                             host, request, observation);
  require_completed(observation);
  require(observation.state.registers[0] == 0x2a,
          "specimen result differs from the architectural result");
  require(observation.state.registers[5] == 0x25,
          "specimen source register changed");
  require(observation.state.nzcv == initial.nzcv,
          "specimen changed preserved condition flags");
  require(oracle::observation_boundary(observation) ==
              "single-instruction-pc+4",
          "specimen did not declare its exact observation boundary");

  auto instruction_expected = initial;
  instruction_expected.registers[0] = 0x2a;
  instruction_expected.pc = guest_pc + sizeof(std::uint32_t);
  const auto observed_digest =
      ::uml::emulator::tests::instruction_evidence::
          architecture_state_digest(observation.state);
  require(observed_digest ==
              ::uml::emulator::tests::instruction_evidence::
                  architecture_state_digest(instruction_expected),
          "native observation did not join the instruction-only expectation");

  auto suffix_projected = observation.state;
  suffix_projected.pc += sizeof(std::uint32_t);
  auto suffix_expected = instruction_expected;
  suffix_expected.pc += sizeof(std::uint32_t);
  require(::uml::emulator::tests::instruction_evidence::
                  architecture_state_digest(suffix_projected) ==
              ::uml::emulator::tests::instruction_evidence::
                  architecture_state_digest(suffix_expected),
          "terminal suffix projection did not preserve instruction state");
}

void evidence_digest_parity() {
  arm64::CpuState initial{};
  for (std::size_t index = 0; index < initial.registers.size(); ++index)
    initial.registers[index] = 0x0102'0304'0506'0708ULL ^ index;
  initial.sp = 0x1122'3344'5566'7788ULL;
  initial.pc = guest_pc;
  initial.nzcv = 0xa000'0000U;
  initial.fpcr = 0x0040'0000U;
  initial.fpsr = 0x0800'0001U;
  initial.tls = 0x8877'6655'4433'2211ULL;
  for (std::size_t reg = 0; reg < initial.vectors.size(); ++reg)
    for (std::size_t byte = 0; byte < initial.vectors[reg].bytes.size(); ++byte)
      initial.vectors[reg].bytes[byte] =
          static_cast<std::byte>(reg * 31U + byte * 19U);

  auto observed = initial;
  observed.registers[0] ^= 0xffff'0000'ffff'0000ULL;
  observed.pc += sizeof(std::uint32_t);
  observed.vectors[31].bytes[15] ^= std::byte{0xff};
  const auto request = baseline_request(0xd503'201fU, initial);
  oracle::Observation observation{
      .state = observed,
  };
  const auto evidence = oracle::observation_digests(request, observation);
  require(evidence.initial_state ==
              ::uml::emulator::tests::instruction_evidence::
                  architecture_state_digest(initial),
          "native evidence initial-state digest differs from engine evidence");
  require(evidence.observed_state ==
              ::uml::emulator::tests::instruction_evidence::
                  architecture_state_digest(observed),
          "native evidence observed-state digest differs from engine evidence");
  const auto empty_memory =
      ::uml::emulator::tests::instruction_evidence::empty_memory_digest();
  require(evidence.initial_memory == empty_memory &&
              evidence.observed_memory == empty_memory,
          "native empty-memory digest differs from engine evidence");

  std::array<std::byte, 20> memory_bytes{};
  for (std::size_t index = 0; index < memory_bytes.size(); ++index)
    memory_bytes[index] = static_cast<std::byte>(index * 17U + 3U);
  const std::array tags{
      oracle::AllocationTag{.offset = 0, .value = 3},
      oracle::AllocationTag{.offset = 16, .value = 7},
  };
  const std::array memory{oracle::MemoryRegion{
      .address = guest_data,
      .initial = memory_bytes,
      .initial_tags = tags,
      .writable = false,
      .guest_code = true,
      .guest_writable = false,
  }};
  auto memory_request = request;
  memory_request.memory = memory;
  auto memory_observation = observation;
  memory_observation.status = oracle::ExecutionStatus::executed;
  memory_observation.memory = {{
      .address = guest_data,
      .bytes = {memory_bytes.begin(), memory_bytes.end()},
      .captured = true,
  }};
  const auto available =
      oracle::observation_digests(memory_request, memory_observation);
  ::uml::emulator::tests::instruction_evidence::Digest expected_initial;
  expected_initial.scalar<std::size_t>(1);
  expected_initial.scalar(guest_data);
  expected_initial.scalar(memory_bytes.size());
  expected_initial.boolean(true);
  expected_initial.boolean(false);
  expected_initial.boolean(false);
  expected_initial.boolean(true);
  expected_initial.boolean(false);
  expected_initial.bytes(memory_bytes.data(), memory_bytes.size());
  expected_initial.scalar<std::uint8_t>(3);
  expected_initial.scalar<std::uint8_t>(7);
  require(available.initial_memory == expected_initial.value(),
          "native initial-memory digest differs from engine evidence");

  auto expected_observed = expected_initial;
  expected_observed = {};
  expected_observed.scalar<std::size_t>(1);
  expected_observed.scalar(guest_data);
  expected_observed.scalar(memory_bytes.size());
  expected_observed.boolean(true);
  expected_observed.boolean(false);
  expected_observed.boolean(false);
  expected_observed.boolean(true);
  expected_observed.boolean(false);
  expected_observed.bytes(memory_bytes.data(), memory_bytes.size());
  expected_observed.scalar<std::uint8_t>(0);
  expected_observed.scalar<std::uint8_t>(0);
  require(available.observed_memory == expected_observed.value(),
          "native observed-memory digest differs from engine evidence");

  memory_observation.memory.front().captured = false;
  require(oracle::observation_digests(memory_request, memory_observation)
              .observed_memory == 0,
          "uncaptured native memory was presented as exact");
}

void complete_register_image(const ::uml::cpu::HostCapabilities &host) {
  arm64::CpuState initial{};
  for (std::size_t index = 0; index < initial.registers.size(); ++index)
    initial.registers[index] =
        0x1020'3040'5060'7080ULL ^ (index * 0x0101'0101'0101'0101ULL);
  initial.registers[18] = 0;
  initial.sp = 0;
  initial.nzcv = 0x6000'0000U;
  initial.fpcr = 0x0040'0000U;
  initial.fpsr = 1U;
  for (std::size_t reg = 0; reg < initial.vectors.size(); ++reg)
    for (std::size_t byte = 0; byte < initial.vectors[reg].bytes.size(); ++byte)
      initial.vectors[reg].bytes[byte] =
          static_cast<std::byte>(reg * 29U + byte * 17U + 3U);

  const auto observation =
      oracle::execute(baseline_request(0xd503'201fU, initial), host);
  require_completed(observation);
  for (std::size_t index = 0; index < initial.registers.size(); ++index) {
    if (index == 18)
      continue;
    require(observation.state.registers[index] == initial.registers[index],
            "no-op did not preserve a general register");
  }
  require(observation.state.sp == initial.sp,
          "zero stack pointer was not captured exactly");
  require(observation.state.nzcv == initial.nzcv,
          "no-op did not preserve condition flags");
  require(observation.state.fpcr == initial.fpcr,
          "no-op did not preserve floating-point control");
  require(observation.state.fpsr == initial.fpsr,
          "no-op did not preserve floating-point status");
  for (std::size_t index = 0; index < initial.vectors.size(); ++index)
    require(observation.state.vectors[index].bytes ==
                initial.vectors[index].bytes,
            "no-op did not preserve a vector register");
}

void vector_execution(const ::uml::cpu::HostCapabilities &host) {
  arm64::CpuState initial{};
  for (std::size_t byte = 0; byte < 16; ++byte) {
    initial.vectors[1].bytes[byte] = static_cast<std::byte>(byte + 1U);
    initial.vectors[2].bytes[byte] = static_cast<std::byte>(2U * byte + 3U);
  }
  const auto observation =
      oracle::execute(baseline_request(0x4e22'8420U, initial), host);
  require_completed(observation);
  for (std::size_t byte = 0; byte < 16; ++byte)
    require(observation.state.vectors[0].bytes[byte] ==
                static_cast<std::byte>(3U * byte + 4U),
            "vector result byte differs from the independent answer");
}

void represented_system_state_execution(
    const ::uml::cpu::HostCapabilities &host) {
  arm64::CpuState initial{};
  initial.nzcv = 0xa000'0000U;
  auto read_flags = oracle::execute(
      baseline_request(0xd53b'4200U, initial), host); // MRS X0, NZCV
  require_completed(read_flags);
  require(read_flags.state.registers[0] == initial.nzcv,
          "condition flags were not readable through native system state");

  initial.registers[1] = 0x5000'0000U;
  auto write_flags = oracle::execute(
      baseline_request(0xd51b'4201U, initial), host); // MSR NZCV, X1
  require_completed(write_flags);
  require(write_flags.state.nzcv == 0x5000'0000U,
          "condition flags were not writable through native system state");

  initial = {};
  initial.fpcr = 0x0040'0000U;
  auto read_control = oracle::execute(
      baseline_request(0xd53b'4402U, initial), host); // MRS X2, FPCR
  require_completed(read_control);
  require(read_control.state.registers[2] == initial.fpcr,
          "floating control was not readable through native system state");

  initial.registers[3] = 0x0080'0000U;
  auto write_control = oracle::execute(
      baseline_request(0xd51b'4403U, initial), host); // MSR FPCR, X3
  require_completed(write_control);
  require(write_control.state.fpcr == 0x0080'0000U,
          "floating control was not writable through native system state");

  initial = {};
  initial.fpsr = 1U;
  auto read_status = oracle::execute(
      baseline_request(0xd53b'4424U, initial), host); // MRS X4, FPSR
  require_completed(read_status);
  require(read_status.state.registers[4] == initial.fpsr,
          "floating status was not readable through native system state");

  initial.registers[5] = 0x0800'0001U;
  auto write_status = oracle::execute(
      baseline_request(0xd51b'4425U, initial), host); // MSR FPSR, X5
  require_completed(write_status);
  require(write_status.state.fpsr == 0x0800'0001U,
          "floating status was not writable through native system state");

  auto invert_carry = baseline_request(0xd500'401fU, initial); // CFINV
  invert_carry.required_features.set(
      static_cast<std::size_t>(::uml::cpu::ArmFeature::flag_m));
  initial.nzcv = 0x8000'0000U;
  invert_carry.initial = initial;
  const auto inverted = oracle::execute(invert_carry, host);
  if (!host.supports(invert_carry.required_features)) {
    require(inverted.eligibility == oracle::Eligibility::missing_feature,
            "unavailable native flag feature did not skip");
    return;
  }
  require_completed(inverted);
  require(inverted.state.nzcv == 0xa000'0000U,
          "native carry inversion did not update captured flags");
}

void memory_execution(const ::uml::cpu::HostCapabilities &host) {
  std::array<std::byte, 16> initial_memory{};
  const auto host_page_size = sysconf(_SC_PAGESIZE);
  require(host_page_size > 0, "cannot query host page size");
  const auto one_past_page =
      guest_data + static_cast<std::uint64_t>(host_page_size);
  arm64::CpuState initial{};
  initial.registers[0] = guest_data;
  initial.registers[1] = guest_data;
  initial.registers[2] = guest_data;
  initial.registers[3] = one_past_page;
  const std::array regions{oracle::MemoryRegion{
      .address = guest_data,
      .initial = initial_memory,
  }};
  auto request = baseline_request(0xf900'0020U, initial);
  request.memory = regions;
  request.input_address_register_mask = (1U << 1U) | (1U << 3U);
  request.output_address_register_mask = (1U << 1U) | (1U << 3U);
  const auto observation = oracle::execute(request, host);
  require_completed(observation);
  require(observation.state.registers[1] == guest_data,
          "address register was not normalized back to the guest value");
  require(observation.state.registers[2] == guest_data,
          "ordinary integer that resembled an address was relocated");
  require(observation.state.registers[3] == one_past_page,
          "one-past address register did not round-trip");
  require(observation.memory.size() == 1 && observation.memory[0].captured,
          "memory result was not captured");
  std::uint64_t stored{};
  std::memcpy(&stored, observation.memory[0].bytes.data(), sizeof(stored));
  require(stored == initial.registers[0],
          "native memory result differs from the stored value");

  auto stack_initial = initial;
  stack_initial.registers[0] = 0xa1b2'c3d4'e5f6'0718ULL;
  stack_initial.sp = guest_data;
  auto stack_request = baseline_request(0xf900'03e0U, stack_initial);
  stack_request.memory = regions;
  stack_request.input_stack_pointer_is_address = true;
  stack_request.output_stack_pointer_is_address = true;
  const auto stack_observation = oracle::execute(stack_request, host);
  require_completed(stack_observation);
  require(stack_observation.state.sp == guest_data,
          "SP memory base was not normalized back to the guest value");
  std::memcpy(&stored, stack_observation.memory[0].bytes.data(),
              sizeof(stored));
  require(stored == stack_initial.registers[0],
          "SP-based native store changed its data operand");

  std::vector<std::byte> post_index_memory(
      static_cast<std::size_t>(host_page_size));
  const std::array post_index_regions{oracle::MemoryRegion{
      .address = guest_data,
      .initial = post_index_memory,
  }};
  auto post_index_initial = initial;
  post_index_initial.registers[0] = 0x8877'6655'4433'2211ULL;
  post_index_initial.registers[1] =
      one_past_page - sizeof(std::uint64_t);
  auto post_index_request =
      baseline_request(0xf80f'f420U, post_index_initial);
  post_index_request.memory = post_index_regions;
  post_index_request.input_address_register_mask = 1U << 1U;
  post_index_request.output_address_register_mask = 1U << 1U;
  const auto post_index = oracle::execute(post_index_request, host);
  require_completed(post_index);
  require(post_index.state.registers[1] == one_past_page + 247U,
          "post-index address beyond the mapped window was not normalized");
  std::memcpy(&stored,
              post_index.memory[0].bytes.data() + host_page_size -
                  sizeof(stored),
              sizeof(stored));
  require(stored == post_index_initial.registers[0],
          "post-index regression did not execute the native store");

  auto relative_request = baseline_request(0x1000'0004U, initial);
  relative_request.output_pc_relative_register_mask = 1U << 4U;
  const auto relative = oracle::execute(relative_request, host);
  require_completed(relative);
  require(relative.state.registers[4] == guest_pc,
          "PC-relative result was not normalized independently of memory");

  auto fault_request = baseline_request(0xf940'0020U, initial);
  fault_request.memory = regions;
  fault_request.initial.registers[1] = one_past_page;
  fault_request.input_address_register_mask = 1U << 1U;
  fault_request.output_address_register_mask = 1U << 1U;
  const auto fault = oracle::execute(fault_request, host);
  require(fault.status == oracle::ExecutionStatus::executed &&
              fault.event == oracle::Event::memory_fault &&
              fault.fault_address == one_past_page &&
              fault.state.registers[1] == one_past_page,
          "one-past memory fault was not normalized to the guest address");
}

void terminal_events(const ::uml::cpu::HostCapabilities &host) {
  arm64::CpuState initial{};
  initial.registers[1] = 1;
  const auto fault =
      oracle::execute(baseline_request(0xf940'0020U, initial), host);
  require(fault.status == oracle::ExecutionStatus::executed &&
              fault.event == oracle::Event::memory_fault &&
              fault.fault_address == 1 && fault.state.pc == guest_pc &&
              fault.retired_instructions == 0 &&
              oracle::observation_boundary(fault) ==
                  "faulting-instruction-pc+0",
          "memory fault did not retain exact event metadata");

  auto invalid_request = baseline_request(0x0000'0000U, initial);
  invalid_request.invalid_encoding = true;
  const auto invalid = oracle::execute(invalid_request, host);
  require(invalid.status == oracle::ExecutionStatus::executed &&
              invalid.event == oracle::Event::illegal_instruction &&
              invalid.state.pc == guest_pc &&
              invalid.retired_instructions == 0,
          "illegal instruction was not classified independently");

  const auto breakpoint =
      oracle::execute(baseline_request(0xd420'2460U, initial), host);
  require(breakpoint.status == oracle::ExecutionStatus::executed &&
              breakpoint.event == oracle::Event::breakpoint &&
              breakpoint.retired_instructions == 1,
          "instruction breakpoint was not distinguished from the sentinel");

  const auto branch =
      oracle::execute(baseline_request(0x1400'0002U, initial), host);
  require(branch.status == oracle::ExecutionStatus::executed &&
              branch.event == oracle::Event::control_target &&
              branch.state.pc == guest_pc + 8U &&
              branch.retired_instructions == 1 &&
              oracle::observation_boundary(branch) ==
                  "single-instruction-control-target",
          "contained control target was not captured");
}

void timeout_is_bounded(const ::uml::cpu::HostCapabilities &host) {
  arm64::CpuState initial{};
  auto request = baseline_request(0x1400'0000U, initial);
  request.timeout = std::chrono::milliseconds{40};
  const auto started = std::chrono::steady_clock::now();
  const auto observation = oracle::execute(request, host);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  require(observation.status == oracle::ExecutionStatus::timed_out,
          "self-loop did not reach the oracle deadline");
  require(oracle::observation_boundary(observation) == "none",
          "timed-out execution declared an observation boundary");
  require(elapsed < std::chrono::seconds{2},
          "oracle timeout was not bounded");
}

void fail_closed_preflight(const ::uml::cpu::HostCapabilities &host) {
  arm64::CpuState initial{};
  auto unsafe = baseline_request(0xd400'0001U, initial);
  unsafe.invalid_encoding = true;
  require(oracle::execute(unsafe, host).eligibility ==
              oracle::Eligibility::unsafe_host_effect,
          "mislabeled host-effecting word passed preflight");

  require(oracle::execute(baseline_request(0xd61f'0000U, initial), host)
                  .eligibility == oracle::Eligibility::unsafe_host_effect,
          "indirect control flow passed preflight");
  require(oracle::execute(baseline_request(0x1400'4000U, initial), host)
                  .eligibility == oracle::Eligibility::unsafe_host_effect,
          "out-of-window direct control flow passed preflight");

  auto platform = baseline_request(0xd503'201fU, initial);
  platform.uses_platform_register = true;
  require(oracle::execute(platform, host).eligibility ==
              oracle::Eligibility::unrepresentable_state,
          "platform-private register contract was executed");

  require(oracle::execute(baseline_request(0xd53b'4212U, initial), host)
                  .eligibility == oracle::Eligibility::unrepresentable_state,
          "represented system access to the platform register was executed");

  auto program = baseline_request(0xd503'201fU, initial);
  program.program_scope = true;
  program.required_features.set(
      static_cast<std::size_t>(::uml::cpu::ArmFeature::xs));
  auto without_program_feature = host;
  without_program_feature.supported.reset(
      static_cast<std::size_t>(::uml::cpu::ArmFeature::xs));
  require(oracle::execute(program, without_program_feature).eligibility ==
              oracle::Eligibility::program_scope,
          "program scope did not take precedence over feature gating");

  auto unsupported_event = baseline_request(0xd503'201fU, initial);
  unsupported_event.requires_unsupported_event = true;
  require(oracle::execute(unsupported_event, host).eligibility ==
              oracle::Eligibility::unsupported_event,
          "unsupported event boundary was executed");

  auto unavailable_geometry = baseline_request(0xd503'201fU, initial);
  unavailable_geometry.requires_unavailable_memory_geometry = true;
  require(oracle::execute(unavailable_geometry, host).eligibility ==
              oracle::Eligibility::memory_geometry,
          "unavailable memory geometry was executed");

  auto literal_context = baseline_request(0x5800'0020U, initial);
  literal_context.requires_unavailable_memory_geometry = true;
  require(oracle::execute(literal_context, host).eligibility ==
              oracle::Eligibility::memory_geometry,
          "literal access to the synthetic sentinel image was executed");

  const auto unlabeled_illegal =
      oracle::execute(baseline_request(0x0000'0000U, initial), host);
  require(unlabeled_illegal.status == oracle::ExecutionStatus::executed &&
              unlabeled_illegal.event ==
                  oracle::Event::illegal_instruction &&
              unlabeled_illegal.eligibility ==
                  oracle::Eligibility::unsupported_instruction &&
              unlabeled_illegal.retired_instructions == 0,
          "runtime SIGILL was not retained as unsupported execution evidence");

  auto missing = baseline_request(0xd503'201fU, initial);
  missing.required_features.set(
      static_cast<std::size_t>(::uml::cpu::ArmFeature::xs));
  auto without_required = host;
  without_required.supported.reset(
      static_cast<std::size_t>(::uml::cpu::ArmFeature::xs));
  require(oracle::execute(missing, without_required).eligibility ==
              oracle::Eligibility::missing_feature,
          "missing exact feature did not skip before execution");

  std::array<std::byte, 8> bytes{};
  const std::array writable_executable{oracle::MemoryRegion{
      .address = guest_data,
      .initial = bytes,
      .writable = true,
      .executable = true,
  }};
  auto writable_executable_request =
      baseline_request(0xd503'201fU, initial);
  writable_executable_request.memory = writable_executable;
  require(oracle::execute(writable_executable_request, host).eligibility ==
              oracle::Eligibility::unsafe_host_effect,
          "writable executable native-oracle memory was accepted");
}

void memory_geometry_is_fail_closed(
    const ::uml::cpu::HostCapabilities &host) {
  std::array<std::byte, 8> bytes{};
  const std::array overlapping{
      oracle::MemoryRegion{.address = guest_data, .initial = bytes},
      oracle::MemoryRegion{.address = guest_data + 4U, .initial = bytes},
  };
  arm64::CpuState initial{};
  auto overlap_request = baseline_request(0xd503'201fU, initial);
  overlap_request.memory = overlapping;
  require(oracle::execute(overlap_request, host).eligibility ==
              oracle::Eligibility::memory_geometry,
          "overlapping memory descriptors were accepted");

  const std::array unaligned{oracle::MemoryRegion{
      .address = guest_data + 1U,
      .initial = bytes,
  }};
  auto exact_request = baseline_request(0xd503'201fU, initial);
  exact_request.memory = unaligned;
  exact_request.requires_exact_memory_boundaries = true;
  require(oracle::execute(exact_request, host).eligibility ==
              oracle::Eligibility::memory_geometry,
          "unrepresentable exact memory boundary was accepted");

  const auto page_size = sysconf(_SC_PAGESIZE);
  require(page_size > 0, "cannot query host page size");
  auto page_end = baseline_request(0xd503'201fU, initial);
  page_end.guest_pc = guest_pc + static_cast<std::uint64_t>(page_size) - 4U;
  const auto page_end_observation = oracle::execute(page_end, host);
  require(page_end_observation.status == oracle::ExecutionStatus::executed &&
              page_end_observation.event == oracle::Event::completed &&
              page_end_observation.state.pc == page_end.guest_pc + 4U,
          "page-end instruction did not reserve the sentinel page");

  auto overflowing_geometry = baseline_request(0xd503'201fU, initial);
  overflowing_geometry.guest_pc =
      std::numeric_limits<std::uint64_t>::max() - 3U;
  require(oracle::execute(overflowing_geometry, host).eligibility ==
              oracle::Eligibility::memory_geometry,
          "near-maximum code geometry overflow was accepted");

  auto overflowing_control = baseline_request(0x15ff'ffffU, initial);
  overflowing_control.guest_pc =
      (std::numeric_limits<std::uint64_t>::max() -
       4U * static_cast<std::uint64_t>(page_size)) &
      ~std::uint64_t{3};
  require(oracle::execute(overflowing_control, host).eligibility ==
              oracle::Eligibility::unsafe_host_effect,
          "near-maximum direct-control overflow was accepted");
}

[[nodiscard]] bool same_action(const struct sigaction &left,
                               const struct sigaction &right) noexcept {
  if (left.sa_handler != right.sa_handler || left.sa_flags != right.sa_flags)
    return false;
  for (int signal = 1; signal < NSIG; ++signal)
    if (sigismember(&left.sa_mask, signal) !=
        sigismember(&right.sa_mask, signal))
      return false;
  return true;
}

[[nodiscard]] bool same_mask(const sigset_t &left,
                             const sigset_t &right) noexcept {
  for (int signal = 1; signal < NSIG; ++signal)
    if (sigismember(&left, signal) != sigismember(&right, signal))
      return false;
  return true;
}

void parent_signal_state_is_preserved(
    const ::uml::cpu::HostCapabilities &host) {
  struct sigaction original_action {};
  sigset_t original_mask{};
  require(sigaction(SIGILL, nullptr, &original_action) == 0 &&
              sigprocmask(SIG_SETMASK, nullptr, &original_mask) == 0,
          "cannot snapshot parent signal state");

  struct sigaction test_action {};
  test_action.sa_handler = parent_signal_handler;
  sigemptyset(&test_action.sa_mask);
  require(sigaction(SIGILL, &test_action, nullptr) == 0,
          "cannot install parent signal test handler");
  sigset_t blocked = original_mask;
  sigaddset(&blocked, SIGILL);
  sigaddset(&blocked, SIGTRAP);
  require(sigprocmask(SIG_SETMASK, &blocked, nullptr) == 0,
          "cannot install parent signal test mask");

  arm64::CpuState initial{};
  const auto observation =
      oracle::execute(baseline_request(0x9100'14a0U, initial), host);
  require_completed(observation);

  struct sigaction current_action {};
  sigset_t current_mask;
  require(sigaction(SIGILL, nullptr, &current_action) == 0 &&
              sigprocmask(SIG_SETMASK, nullptr, &current_mask) == 0 &&
              same_action(current_action, test_action) &&
              same_mask(current_mask, blocked) && parent_signal_count == 0,
          "oracle changed the parent signal disposition or mask");
  require(sigaction(SIGILL, &original_action, nullptr) == 0 &&
              sigprocmask(SIG_SETMASK, &original_mask, nullptr) == 0,
          "cannot restore parent signal state");
}

} // namespace

int main() {
  const auto &host =
      ::uml::platform::apple::probed_host_capabilities();
  allocation_identity_and_constraint_contract();
  ordinary_lookup_does_not_load_catalogs();
  materialization_lookup_contract();
  exact_arithmetic_specimen(host);
  evidence_digest_parity();
  complete_register_image(host);
  vector_execution(host);
  represented_system_state_execution(host);
  memory_execution(host);
  terminal_events(host);
  timeout_is_bounded(host);
  fail_closed_preflight(host);
  memory_geometry_is_fail_closed(host);
  parent_signal_state_is_preserved(host);
  return EXIT_SUCCESS;
}
