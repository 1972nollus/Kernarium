// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_instruction_materialization.hpp"
#include "instruction_evidence_digest.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <source_location>
#include <string>
#include <string_view>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

namespace {

using namespace uml::emulator::tests;
constexpr unsigned repeated_contracts = 1024U;

[[noreturn]] void fail(const char *message) {
  std::fprintf(stderr, "instruction-materialization-test: %s\n", message);
  std::exit(1);
}

void configure_child(const std::string &base) {
  const auto materialization = base + ".inputs.jsonl.gz";
  std::error_code error;
  const std::filesystem::path requested{base};
  const auto canonical_base =
      (std::filesystem::canonical(requested.parent_path(), error) /
       requested.filename())
          .string();
  if (error)
    fail("cannot canonicalize child evidence path");
  const std::string marker =
      "v1\nmaterialization-contract\n" + canonical_base;
  if (setenv("UML_INSTRUCTION_RAW_EVIDENCE", "1", 1) != 0 ||
      setenv("UML_INSTRUCTION_EVIDENCE", canonical_base.c_str(), 1) != 0 ||
      setenv("UML_INSTRUCTION_INPUT_MATERIALIZATION",
             materialization.c_str(), 1) != 0 ||
      setenv("UML_INSTRUCTION_EVIDENCE_TEST", "materialization-contract", 1) !=
          0 ||
      setenv("UML_INSTRUCTION_EVIDENCE_LOCKED", marker.c_str(), 1) != 0)
    fail("cannot configure child environment");
}

void configure_raw_only_child(const std::string &base) {
  std::error_code error;
  const std::filesystem::path requested{base};
  const auto canonical_base =
      (std::filesystem::canonical(requested.parent_path(), error) /
       requested.filename())
          .string();
  if (error)
    fail("cannot canonicalize raw-only child evidence path");
  const std::string marker =
      "v1\nmaterialization-contract\n" + canonical_base;
  if (setenv("UML_INSTRUCTION_RAW_EVIDENCE", "1", 1) != 0 ||
      setenv("UML_INSTRUCTION_EVIDENCE", canonical_base.c_str(), 1) != 0 ||
      unsetenv("UML_INSTRUCTION_INPUT_MATERIALIZATION") != 0 ||
      setenv("UML_INSTRUCTION_EVIDENCE_TEST", "materialization-contract", 1) !=
          0 ||
      setenv("UML_INSTRUCTION_EVIDENCE_LOCKED", marker.c_str(), 1) != 0)
    fail("cannot configure raw-only child environment");
}

[[nodiscard]] InstructionInputMaterialization make_record(
    std::span<const ProgramWord> code, std::span<const std::byte> page,
    const uml::cpu::arm64::CpuState &state,
    std::span<const std::uint32_t> evidence_words) {
  return {
      .case_name = "duplicate-contract",
      .code = code,
      .entry_offset = 0,
      .evidence_words = evidence_words,
      .installed_code_page = page,
      .initial = state,
      .expected = state,
      .memory = {},
      .expected_event = "system-call",
      .expected_retirement = "exact:2",
      .instruction_budget = 64,
      .expected_state_oracle = "exact",
      .expected_fault = "none",
      .native = NativeRequirements::baseline(),
      .engines = {
          .interpreter = InstructionEngineResult::executed_retired,
          .gadgets = InstructionEngineResult::executed_retired,
          .jit = InstructionEngineResult::executed_retired,
      },
      .native_observation = {},
  };
}

[[nodiscard]] std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE>
code_page(std::uint32_t instruction) {
  std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> page{};
  for (std::size_t byte = 0; byte < sizeof(instruction); ++byte)
    page[byte] = static_cast<std::byte>(instruction >> (byte * 8U));
  return page;
}

void emit_duplicate(const std::string &base) {
  configure_child(base);
  constexpr std::uint32_t instruction = 0xd503201fU;
  constexpr std::array evidence_words{instruction};
  constexpr std::array code{ProgramWord{.offset = 0,
                                        .instruction = instruction}};
  const auto page = code_page(instruction);
  uml::cpu::arm64::CpuState state{};
  auto record = make_record(code, page, state, evidence_words);
  const auto location = std::source_location::current();
  const auto oracle =
      std::string{"custom-validator@tests/unit/arm64/"
                  "arm64_instruction_materialization_tests.cpp:"} +
      std::to_string(location.line());
  record.expected_state_oracle = oracle;
  record.has_custom_validator = true;
  record.has_before_engine_hook = true;
  record.validator_relation_id = "exact-increment/v1";
  record.before_engine_effect_id = "install-test-state/v1";
  for (unsigned invocation = 0; invocation < repeated_contracts; ++invocation)
    record_instruction_input_materialization(record, location);
  std::exit(0);
}

void emit_raw_only(const std::string &base) {
  configure_raw_only_child(base);
  constexpr std::uint32_t instruction = 0xd503201fU;
  constexpr std::array evidence_words{instruction};
  constexpr std::array code{ProgramWord{.offset = 0,
                                        .instruction = instruction}};
  const auto page = code_page(instruction);
  uml::cpu::arm64::CpuState state{};
  const auto record = make_record(code, page, state, evidence_words);
  record_instruction_input_materialization(record,
                                           std::source_location::current());
  std::exit(0);
}

void emit_shared_evidence_programs(const std::string &base) {
  configure_child(base);
  constexpr std::uint32_t instruction = 0xd503201fU;
  constexpr std::array evidence_words{instruction};
  constexpr std::uint32_t first_suffix = 0xd503205fU;
  constexpr std::uint32_t second_suffix = 0xd503209fU;
  constexpr std::array first_code{
      ProgramWord{.offset = 0, .instruction = instruction},
      ProgramWord{.offset = 4, .instruction = first_suffix},
  };
  constexpr std::array second_code{
      ProgramWord{.offset = 0, .instruction = instruction},
      ProgramWord{.offset = 4, .instruction = second_suffix},
  };
  auto first_page = code_page(instruction);
  auto second_page = first_page;
  for (std::size_t byte = 0; byte < sizeof(first_suffix); ++byte) {
    first_page[4U + byte] =
        static_cast<std::byte>(first_suffix >> (byte * 8U));
    second_page[4U + byte] =
        static_cast<std::byte>(second_suffix >> (byte * 8U));
  }
  uml::cpu::arm64::CpuState state{};
  const auto first =
      make_record(first_code, first_page, state, evidence_words);
  const auto second =
      make_record(second_code, second_page, state, evidence_words);
  const auto location = std::source_location::current();
  record_instruction_input_materialization(first, location);
  record_instruction_input_materialization(second, location);
  std::exit(0);
}

void emit_corrupt_page(const std::string &base) {
  configure_child(base);
  constexpr std::uint32_t instruction = 0xd503201fU;
  constexpr std::array evidence_words{instruction};
  constexpr std::array code{ProgramWord{.offset = 0,
                                        .instruction = instruction}};
  auto page = code_page(instruction);
  page[8] = std::byte{1};
  uml::cpu::arm64::CpuState state{};
  const auto record = make_record(code, page, state, evidence_words);
  record_instruction_input_materialization(record,
                                           std::source_location::current());
  fail("non-sparse code-page byte was accepted");
}

[[nodiscard]] std::string escaped_output_path(const std::string &base) {
  const auto evidence_directory = std::filesystem::path{base}.parent_path();
  return (evidence_directory.parent_path() /
          (evidence_directory.filename().string() +
           ".escaped.inputs.jsonl.gz"))
      .string();
}

void emit_escaped_output(const std::string &base) {
  configure_child(base);
  const auto escaped = escaped_output_path(base);
  if (setenv("UML_INSTRUCTION_INPUT_MATERIALIZATION", escaped.c_str(), 1) !=
      0)
    fail("cannot mutate child materialization output");
  constexpr std::uint32_t instruction = 0xd503201fU;
  constexpr std::array evidence_words{instruction};
  constexpr std::array code{ProgramWord{.offset = 0,
                                        .instruction = instruction}};
  const auto page = code_page(instruction);
  uml::cpu::arm64::CpuState state{};
  const auto record = make_record(code, page, state, evidence_words);
  record_instruction_input_materialization(record,
                                           std::source_location::current());
  fail("escaped materialization output was accepted");
}

[[nodiscard]] std::string interrupted_ready_path(const std::string &base) {
  return base + ".interrupted.ready";
}

void emit_interrupted_output(const std::string &base) {
  configure_child(base);
  constexpr std::uint32_t instruction = 0xd503201fU;
  constexpr std::array evidence_words{instruction};
  constexpr std::array code{ProgramWord{.offset = 0,
                                        .instruction = instruction}};
  const auto page = code_page(instruction);
  uml::cpu::arm64::CpuState state{};
  const auto record = make_record(code, page, state, evidence_words);
  record_instruction_input_materialization(record,
                                           std::source_location::current());
  auto *ready = std::fopen(interrupted_ready_path(base).c_str(), "w");
  if (ready == nullptr || std::fputs("ready\n", ready) < 0 ||
      std::fclose(ready) != 0)
    fail("cannot publish interrupted-child readiness");
  if (raise(SIGSTOP) != 0)
    fail("cannot stop interrupted materialization child");
  fail("interrupted materialization child resumed unexpectedly");
}

void emit_different_state(const std::string &base) {
  configure_child(base);
  constexpr std::uint32_t instruction = 0xd503201fU;
  constexpr std::array evidence_words{instruction};
  constexpr std::array code{ProgramWord{.offset = 0,
                                        .instruction = instruction}};
  const auto page = code_page(instruction);
  uml::cpu::arm64::CpuState first_state{};
  uml::cpu::arm64::CpuState second_state{};
  second_state.registers[0] = 1U;
  auto first = make_record(code, page, first_state, evidence_words);
  auto second = make_record(code, page, second_state, evidence_words);
  first.case_name = "first-state";
  second.case_name = "second-state";
  const auto location = std::source_location::current();
  record_instruction_input_materialization(first, location);
  record_instruction_input_materialization(second, location);
  std::exit(0);
}

void emit_different_budget(const std::string &base) {
  configure_child(base);
  constexpr std::uint32_t instruction = 0xd503201fU;
  constexpr std::array evidence_words{instruction};
  constexpr std::array code{ProgramWord{.offset = 0,
                                        .instruction = instruction}};
  const auto page = code_page(instruction);
  uml::cpu::arm64::CpuState state{};
  auto first = make_record(code, page, state, evidence_words);
  auto second = first;
  second.instruction_budget = 63;
  const auto location = std::source_location::current();
  record_instruction_input_materialization(first, location);
  record_instruction_input_materialization(second, location);
  std::exit(0);
}

[[nodiscard]] std::string read_payload(const std::string &base) {
  auto *input = gzopen((base + ".inputs.jsonl.gz").c_str(), "rb");
  if (input == nullptr)
    fail("cannot read materialization event stream");
  std::string payload;
  std::array<char, 65536> buffer{};
  for (;;) {
    const auto size = gzread(input, buffer.data(), buffer.size());
    if (size < 0)
      fail("cannot decompress materialization event stream");
    if (size == 0)
      break;
    payload.append(buffer.data(), static_cast<std::size_t>(size));
  }
  if (gzclose(input) != Z_OK || payload.empty())
    fail("cannot finish materialization event stream");
  return payload;
}

[[nodiscard]] std::size_t count(std::string_view text,
                                std::string_view needle) {
  std::size_t result = 0;
  std::size_t offset = 0;
  while ((offset = text.find(needle, offset)) != std::string_view::npos) {
    ++result;
    offset += needle.size();
  }
  return result;
}

[[nodiscard]] int wait_for_child(pid_t child) {
  int status{};
  while (waitpid(child, &status, 0) == -1) {
    if (errno != EINTR)
      fail("waitpid failed");
  }
  return status;
}

void wait_for_file(const std::string &path) {
  for (unsigned attempt = 0; attempt < 500U; ++attempt) {
    if (std::filesystem::exists(path))
      return;
    usleep(10'000);
  }
  fail("timed out waiting for child readiness");
}

[[nodiscard]] pid_t spawn(void (*body)(const std::string &),
                          const std::string &base) {
  const auto child = fork();
  if (child == -1)
    fail("fork failed");
  if (child == 0)
    body(base);
  return child;
}

} // namespace

int main() {
  const auto *outer_environment =
      std::getenv("UML_INSTRUCTION_INPUT_MATERIALIZATION");
  const std::filesystem::path outer_materialization =
      outer_environment == nullptr ? std::filesystem::path{}
                                   : std::filesystem::path{outer_environment};
  if (!outer_materialization.empty() &&
      std::filesystem::exists(outer_materialization))
    fail("outer producer sidecar exists before the private fixture");

  if (instruction_evidence::sha256_hex("") !=
          "e3b0c44298fc1c149afbf4c8996fb924"
          "27ae41e4649b934ca495991b7852b855" ||
      instruction_evidence::sha256_hex("abc") !=
          "ba7816bf8f01cfea414140de5dae2223"
          "b00361a396177a9cb410ff61f20015ad")
    fail("test-only SHA-256 known vectors differ");

  char directory_template[] = "/tmp/uml-materialization-test.XXXXXX";
  const auto *directory = mkdtemp(directory_template);
  if (directory == nullptr)
    fail("mkdtemp failed");
  const std::string base = std::string{directory} + "/evidence.tsv";
  const std::string fixture_lock_path = std::string{directory} + ".lock";
  const auto fixture_lock =
      open(fixture_lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fixture_lock < 0 || flock(fixture_lock, LOCK_EX | LOCK_NB) != 0)
    fail("cannot lock private test fixture");

  const auto escaped_output = escaped_output_path(base);
  const auto escaped_status =
      wait_for_child(spawn(emit_escaped_output, base));
  if (!WIFSIGNALED(escaped_status) ||
      WTERMSIG(escaped_status) != SIGABRT ||
      std::filesystem::exists(escaped_output))
    fail("escaped child materialization output did not abort safely");

  const auto interrupted_ready = interrupted_ready_path(base);
  const auto interrupted = spawn(emit_interrupted_output, base);
  wait_for_file(interrupted_ready);
  const auto materialization_path = base + ".inputs.jsonl.gz";
  if (std::filesystem::exists(materialization_path))
    fail("interrupted child exposed a partial materialization output");
  if (kill(interrupted, SIGKILL) != 0)
    fail("cannot terminate interrupted materialization child");
  const auto interrupted_status = wait_for_child(interrupted);
  if (!WIFSIGNALED(interrupted_status) ||
      WTERMSIG(interrupted_status) != SIGKILL ||
      std::filesystem::exists(materialization_path))
    fail("interrupted child published a materialization output");
  if (unlink(interrupted_ready.c_str()) != 0)
    fail("cannot remove interrupted-child readiness");

  const auto raw_only_status = wait_for_child(spawn(emit_raw_only, base));
  if (!WIFEXITED(raw_only_status) || WEXITSTATUS(raw_only_status) != 0 ||
      std::filesystem::exists(materialization_path))
    fail("raw evidence implicitly enabled input materialization");

  const auto duplicate_status = wait_for_child(spawn(emit_duplicate, base));
  if (!WIFEXITED(duplicate_status) || WEXITSTATUS(duplicate_status) != 0)
    fail("duplicate-record child failed");
  if (std::filesystem::exists(materialization_path + ".producer.tmp"))
    fail("successful materialization left a producer staging file");

  const auto payload = read_payload(base);
  if (payload.size() >= 4U * 1024U * 1024U ||
      std::filesystem::file_size(base + ".inputs.jsonl.gz") >=
          1U * 1024U * 1024U ||
      count(payload, "\"schema\":\"uml.a64.program-blob/v2\"") != 1U ||
      count(payload, "\"schema\":\"uml.a64.state-blob/v2\"") != 1U ||
      count(payload, "\"schema\":\"uml.a64.memory-blob/v2\"") != 1U ||
      count(payload, "\"schema\":\"uml.a64.input-contract/v4\"") !=
          repeated_contracts ||
      count(payload, "\"raw_occurrences\":\"0000000000000001\"") !=
          repeated_contracts ||
      payload.find("installed_code_page") != std::string::npos ||
      payload.find("zero-fill-sparse-words/v1") == std::string::npos ||
      payload.find("reconstructed_page_sha256") == std::string::npos ||
      payload.find("\"engine_results\"") == std::string::npos ||
      payload.find("\"exclusive_monitor\"") == std::string::npos ||
      payload.find("\"obligations\":[]") == std::string::npos ||
      payload.find("\"relation_id\":\"exact-increment/v1\"") ==
          std::string::npos ||
      payload.find("\"effect_id\":\"install-test-state/v1\"") ==
          std::string::npos)
    fail("bounded content-addressed event stream is incomplete");

  const auto different_status =
      wait_for_child(spawn(emit_different_state, base));
  if (!WIFEXITED(different_status) || WEXITSTATUS(different_status) != 0)
    fail("different-state child failed");
  const auto different = read_payload(base);
  if (count(different, "\"schema\":\"uml.a64.program-blob/v2\"") != 1U ||
      count(different, "\"schema\":\"uml.a64.state-blob/v2\"") != 2U ||
      count(different, "\"schema\":\"uml.a64.memory-blob/v2\"") != 1U ||
      count(different, "\"schema\":\"uml.a64.input-contract/v4\"") != 2U)
    fail("repeated program/different-state blob factoring differs");

  const auto budget_status =
      wait_for_child(spawn(emit_different_budget, base));
  if (!WIFEXITED(budget_status) || WEXITSTATUS(budget_status) != 0)
    fail("different-budget child failed");
  const auto budgets = read_payload(base);
  if (count(budgets, "\"schema\":\"uml.a64.input-contract/v4\"") != 2U ||
      count(budgets, "\"instruction_budget\":\"0000000000000040\"") !=
          1U ||
      count(budgets, "\"instruction_budget\":\"000000000000003f\"") !=
          1U)
    fail("instruction budget is not bound into distinct input contracts");

  const auto shared_program_status =
      wait_for_child(spawn(emit_shared_evidence_programs, base));
  if (!WIFEXITED(shared_program_status) ||
      WEXITSTATUS(shared_program_status) != 0)
    fail("shared-evidence/different-program child failed");
  const auto shared_programs = read_payload(base);
  if (count(shared_programs,
            "\"schema\":\"uml.a64.program-blob/v2\"") != 2U ||
      count(shared_programs,
            "\"schema\":\"uml.a64.state-blob/v2\"") != 1U ||
      count(shared_programs,
            "\"schema\":\"uml.a64.input-contract/v4\"") != 2U)
    fail("shared raw evidence did not preserve both exact programs");

  const auto corrupt_page_status =
      wait_for_child(spawn(emit_corrupt_page, base));
  if (!WIFSIGNALED(corrupt_page_status) ||
      WTERMSIG(corrupt_page_status) != SIGABRT)
    fail("non-sparse code page did not abort");
  if (read_payload(base) != shared_programs)
    fail("aborted materialization replaced the canonical output");
  const auto stale_staging = materialization_path + ".producer.tmp";
  if (!std::filesystem::exists(stale_staging) ||
      unlink(stale_staging.c_str()) != 0)
    fail("aborted materialization did not leave a private staging file");

  if (unlink((base + ".inputs.jsonl.gz").c_str()) != 0 ||
      rmdir(directory) != 0)
    fail("cannot remove private test fixture");
  if (close(fixture_lock) != 0 || unlink(fixture_lock_path.c_str()) != 0)
    fail("cannot remove private test fixture lock");
  if (!outer_materialization.empty() &&
      std::filesystem::exists(outer_materialization))
    fail("private fixture created an outer producer sidecar");
  return 0;
}
