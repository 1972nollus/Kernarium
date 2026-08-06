// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/host_capabilities.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace uml::emulator::tests::native_oracle {

enum class Eligibility : std::uint8_t {
  eligible,
  missing_feature,
  unsupported_host,
  unsafe_host_effect,
  unrepresentable_state,
  unsupported_instruction,
  unsupported_event,
  program_scope,
  memory_geometry,
};

enum class ExecutionStatus : std::uint8_t {
  executed,
  skipped,
  timed_out,
  setup_failed,
  protocol_failed,
};

enum class Event : std::uint8_t {
  none,
  completed,
  illegal_instruction,
  breakpoint,
  memory_fault,
  floating_point_trap,
  control_target,
};

struct AllocationTag final {
  std::size_t offset{};
  std::uint8_t value{};
};

struct MemoryRegion final {
  std::uint64_t address{};
  std::span<const std::byte> initial;
  std::span<const AllocationTag> initial_tags;
  bool readable{true};
  bool writable{true};
  bool executable{};
  bool capture_after_execution{true};
  bool guest_code{};
  bool guest_writable{true};
};

struct Request final {
  std::string_view oracle_case_id;
  std::uint32_t instruction{};
  std::uint64_t guest_pc{};
  ::uml::cpu::arm64::CpuState initial;
  ::uml::cpu::ArmFeatureSet required_features;
  std::span<const MemoryRegion> memory;
  std::chrono::milliseconds timeout{250};
  bool invalid_encoding{};
  bool program_scope{};
  bool uses_platform_register{};
  bool requires_unavailable_system_state{};
  bool requires_unsupported_instruction{};
  bool requires_unsupported_event{};
  bool requires_unavailable_memory_geometry{};
  bool requires_exact_memory_boundaries{};
  std::uint32_t input_address_register_mask{};
  std::uint32_t output_address_register_mask{};
  std::uint32_t output_pc_relative_register_mask{};
  bool input_stack_pointer_is_address{};
  bool output_stack_pointer_is_address{};
};

struct MemoryObservation final {
  std::uint64_t address{};
  std::vector<std::byte> bytes;
  bool captured{};
};

struct Observation final {
  Eligibility eligibility{Eligibility::unsupported_host};
  ExecutionStatus status{ExecutionStatus::skipped};
  Event event{Event::none};
  int signal{};
  std::uint64_t fault_address{};
  ::uml::cpu::arm64::CpuState state;
  std::vector<MemoryObservation> memory;
  bool platform_register_changed{};
  std::size_t retired_instructions{};
};

[[nodiscard]] Observation execute(
    const Request &request,
    const ::uml::cpu::HostCapabilities &observed_capabilities) noexcept;

[[nodiscard]] constexpr std::string_view name(Eligibility value) noexcept {
  switch (value) {
  case Eligibility::eligible:
    return "eligible";
  case Eligibility::missing_feature:
    return "missing-feature";
  case Eligibility::unsupported_host:
    return "unsupported-host";
  case Eligibility::unsafe_host_effect:
    return "unsafe-host-effect";
  case Eligibility::unrepresentable_state:
    return "unrepresentable-state";
  case Eligibility::unsupported_instruction:
    return "unsupported-instruction";
  case Eligibility::unsupported_event:
    return "unsupported-event";
  case Eligibility::program_scope:
    return "program-scope";
  case Eligibility::memory_geometry:
    return "memory-geometry";
  }
  return "invalid";
}

[[nodiscard]] constexpr std::string_view name(ExecutionStatus value) noexcept {
  switch (value) {
  case ExecutionStatus::executed:
    return "executed";
  case ExecutionStatus::skipped:
    return "skipped";
  case ExecutionStatus::timed_out:
    return "timed-out";
  case ExecutionStatus::setup_failed:
    return "setup-failed";
  case ExecutionStatus::protocol_failed:
    return "protocol-failed";
  }
  return "invalid";
}

[[nodiscard]] constexpr std::string_view name(Event value) noexcept {
  switch (value) {
  case Event::none:
    return "none";
  case Event::completed:
    return "completed";
  case Event::illegal_instruction:
    return "illegal-instruction";
  case Event::breakpoint:
    return "breakpoint";
  case Event::memory_fault:
    return "memory-fault";
  case Event::floating_point_trap:
    return "floating-point-trap";
  case Event::control_target:
    return "control-target";
  }
  return "invalid";
}

} // namespace uml::emulator::tests::native_oracle
