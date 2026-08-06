// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uml::emulator::tests::native_oracle {

struct AllocationIdentity final {
  std::string_view stable_id;
  std::string_view architecture_file;
  std::string_view architecture_encoding;
  std::string_view architecture_mask;
  std::string_view architecture_value;
  std::string_view architecture_constraints;
  std::string_view decode_class;
  std::string_view decode_encoding;
  std::string_view decode_mask;
  std::string_view decode_value;
  std::string_view decode_constraints;
};

[[nodiscard]] std::string
allocation_status_id(const AllocationIdentity &identity);

[[nodiscard]] bool allocation_encoding_matches(
    std::uint32_t instruction, std::uint32_t architecture_mask,
    std::uint32_t architecture_value,
    std::string_view architecture_constraints, std::uint32_t decode_mask,
    std::uint32_t decode_value,
    std::string_view decode_constraints) noexcept;

enum class SelectionScope : std::uint8_t {
  instruction,
  program,
};

struct AllocationMaterialization final {
  std::string aggregate_id;
  std::string decode_partition_id;
  std::string allocation_status_id;
  std::string declared_digest;
  std::string observed_digest;
  std::string architecture_features;
  std::string producer;
  std::string source_site;
  std::string source_file;
  std::uint_least32_t source_line{};
  SelectionScope scope{SelectionScope::instruction};
  std::uint32_t guest_page_size{};
};

[[nodiscard]] bool materialization_record_is_well_formed(
    const AllocationMaterialization &record) noexcept;

struct AllocationSelection final {
  /* Representative allocation status is consumed once per process. */
  std::vector<std::string> allocation_status_ids;
};

/*
 * Evidence collection is intentionally absent from ordinary unit runs.  When
 * UML_NATIVE_HARDWARE_EVIDENCE is enabled alongside the ordinary instruction
 * evidence variables, this process-local selector reads the reviewed
 * allocation and aggregate catalogs, assigns one deterministic source site to
 * every official allocation, and admits that allocation once.
 */
[[nodiscard]] AllocationSelection select_native_allocations(
    std::uint32_t instruction, SelectionScope scope,
    const std::source_location &location);

/* Return every current-page allocated-valid aggregate candidate for this raw
 * source and word. This lookup never consumes either candidates or the single
 * native representative; the exact raw-state join is performed downstream. */
[[nodiscard]] std::vector<AllocationMaterialization>
lookup_native_materializations(std::uint32_t instruction,
                               SelectionScope scope,
                               const std::source_location &location);

} // namespace uml::emulator::tests::native_oracle
