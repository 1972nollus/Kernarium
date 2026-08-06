// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "native_hardware_oracle.hpp"

#include <cstdint>
#include <source_location>
#include <string_view>

namespace uml::emulator::tests::native_oracle {

struct ObservationDigests final {
  std::uint64_t initial_state{};
  std::uint64_t initial_memory{};
  std::uint64_t observed_state{};
  std::uint64_t observed_memory{};
};

[[nodiscard]] ObservationDigests
observation_digests(const Request &request,
                    const Observation &observation) noexcept;

[[nodiscard]] std::string_view
observation_boundary(const Observation &observation) noexcept;

void record_observation(
    std::string_view oracle_case_id, std::string_view case_name,
    std::uint32_t instruction, const ::uml::cpu::ArmFeatureSet &required,
    const ::uml::cpu::HostCapabilities &observed, const Request &request,
    const Observation &observation, std::uint64_t occurrences = 1,
    const std::source_location &location = std::source_location::current());

} // namespace uml::emulator::tests::native_oracle
