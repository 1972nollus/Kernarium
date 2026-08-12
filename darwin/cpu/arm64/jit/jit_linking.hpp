// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace uml::cpu::arm64 {

enum class JitLinkCapability : std::uint8_t {
  store_publication = 1U << 0U,
  fp_state_materialized = 1U << 1U,
  native_fp_environment = 1U << 2U,
  native_fpmr_environment = 1U << 3U,
};

[[nodiscard]] constexpr std::uint8_t
link_capability(JitLinkCapability capability) noexcept {
  return static_cast<std::uint8_t>(capability);
}

constexpr std::array<std::uint8_t, 10> jit_link_selector_capabilities{
    0,
    link_capability(JitLinkCapability::store_publication),
    link_capability(JitLinkCapability::fp_state_materialized),
    link_capability(JitLinkCapability::store_publication) |
        link_capability(JitLinkCapability::fp_state_materialized),
    link_capability(JitLinkCapability::fp_state_materialized) |
        link_capability(JitLinkCapability::native_fp_environment),
    link_capability(JitLinkCapability::store_publication) |
        link_capability(JitLinkCapability::fp_state_materialized) |
        link_capability(JitLinkCapability::native_fp_environment),
    link_capability(JitLinkCapability::fp_state_materialized) |
        link_capability(JitLinkCapability::native_fpmr_environment),
    link_capability(JitLinkCapability::store_publication) |
        link_capability(JitLinkCapability::fp_state_materialized) |
        link_capability(JitLinkCapability::native_fpmr_environment),
    link_capability(JitLinkCapability::fp_state_materialized) |
        link_capability(JitLinkCapability::native_fp_environment) |
        link_capability(JitLinkCapability::native_fpmr_environment),
    link_capability(JitLinkCapability::store_publication) |
        link_capability(JitLinkCapability::fp_state_materialized) |
        link_capability(JitLinkCapability::native_fp_environment) |
        link_capability(JitLinkCapability::native_fpmr_environment),
};
constexpr std::size_t jit_link_selector_count =
    jit_link_selector_capabilities.size();
constexpr std::uint8_t invalid_link_selector = 0xffU;

[[nodiscard]] constexpr bool
valid_link_capabilities(std::uint8_t capabilities) noexcept {
  const auto materialized =
      link_capability(JitLinkCapability::fp_state_materialized);
  const auto native =
      link_capability(JitLinkCapability::native_fp_environment) |
      link_capability(JitLinkCapability::native_fpmr_environment);
  return (capabilities & native) == 0U ||
         (capabilities & materialized) != 0U;
}

[[nodiscard]] constexpr std::uint8_t
link_selector_for_capabilities(std::uint8_t capabilities) noexcept {
  for (std::size_t selector = 0;
       selector < jit_link_selector_capabilities.size(); ++selector)
    if (jit_link_selector_capabilities[selector] == capabilities)
      return static_cast<std::uint8_t>(selector);
  return invalid_link_selector;
}

[[nodiscard]] std::uint8_t
checked_link_selector(std::uint8_t capabilities) noexcept;

[[nodiscard]] constexpr std::uint8_t
link_state_requirements(bool accesses_fp_state, bool native_fp,
                        bool native_fpmr) noexcept {
  std::uint8_t requirements = 0;
  if (accesses_fp_state || native_fp || native_fpmr)
    requirements |=
        link_capability(JitLinkCapability::fp_state_materialized);
  if (native_fp)
    requirements |=
        link_capability(JitLinkCapability::native_fp_environment);
  if (native_fpmr)
    requirements |=
        link_capability(JitLinkCapability::native_fpmr_environment);
  return requirements;
}

[[nodiscard]] constexpr bool
valid_link_state_metadata(bool accesses_fp_state, bool native_fp,
                          bool native_fpmr) noexcept {
  return accesses_fp_state || (!native_fp && !native_fpmr);
}

struct alignas(128) JitBlockLink final {
  /*
   * Generated code uses raw internal-entry branch targets.  The initial C++
   * call owns the one native stack frame for the complete chain; linked
   * successors enter after their ordinary external prologue and preserve that
   * frame until a cold, terminal, or fairness exit unwinds it exactly once.
   *
   * The host-owned selector describes setup performed by the initial C++
   * entry. A zero cell returns to C++ before a successor whose store or
   * architectural-state requirements have not been established.
   *
   * The ten reachable setup combinations and their metadata fit in one
   * 128-byte record. Generated code points at entries and reads the preceding
   * metadata with one aligned pair load.
   */
  std::uint64_t guest_pc{};
  std::uint64_t instruction_count{};
  std::array<std::uintptr_t, jit_link_selector_count> entries{};
  std::uint64_t activation_epoch{};
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  std::uint32_t profile_read_before_write{};
  bool profile_mask_valid{};
#endif
};

struct JitIndirectLinkEntry final {
  std::uint64_t guest_pc{};
  const std::uintptr_t *link_entries{};
};

struct JitIndirectLink final {
  std::array<JitIndirectLinkEntry, 2> entries{};
  std::uint64_t source_pc{};
  std::uint64_t activation_epoch{};
  std::uint8_t next_replacement{};
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  /*
   * Generated PIC data remains exactly {guest PC, entry-table pointer}.
   * Diagnostic attribution retains the owning records out of that hot array.
   */
  std::array<const JitBlockLink *, 2> profile_targets{};
#endif
};

/*
 * Generated code embeds pointers to JitBlockLink entry arrays, so records must
 * not move or be reclaimed while the code arena is live. Bound the stable
 * storage instead: once full, newly compiled branches use the existing
 * unlinked exit path.
 */
class JitBlockLinkTable final {
public:
  explicit JitBlockLinkTable(std::size_t capacity);
  ~JitBlockLinkTable();

  JitBlockLinkTable(const JitBlockLinkTable &) = delete;
  JitBlockLinkTable &operator=(const JitBlockLinkTable &) = delete;

  [[nodiscard]] JitBlockLink *find_or_create(std::uint64_t pc);
  void clear() noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  /* Exact requested backing storage plus conservative allocator metadata.
   * The table uses a fixed record array and a 50%-load open-address index, so
   * this value cannot grow as records are populated.
   */
  [[nodiscard]] static std::size_t
  allocation_bytes(std::size_t capacity) noexcept;

private:
  std::size_t capacity_{};
  std::size_t size_{};
  std::size_t index_capacity_{};
  JitBlockLink *records_{};
  std::unique_ptr<std::uint32_t[]> index_;
};

/*
 * Generated indirect exits embed pointers to these cells. Keep every cell
 * stable for the code arena's lifetime and bound their total number. Guest
 * targets are comparison data only; entries point exclusively to host-owned
 * JitBlockLink entry arrays, never to native code supplied by the guest.
 */
class JitIndirectLinkTable final {
public:
  JitIndirectLinkTable(std::size_t capacity,
                       const JitBlockLink *empty_link);
  ~JitIndirectLinkTable();

  JitIndirectLinkTable(const JitIndirectLinkTable &) = delete;
  JitIndirectLinkTable &operator=(const JitIndirectLinkTable &) = delete;

  [[nodiscard]] JitIndirectLink *find_or_create(std::uint64_t source_pc);
  [[nodiscard]] JitIndirectLink *find(
      std::uint64_t source_pc) const noexcept;
  void reset(JitIndirectLink &site) const noexcept;
  void clear() noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  [[nodiscard]] static std::size_t
  allocation_bytes(std::size_t capacity) noexcept;

private:
  std::size_t capacity_{};
  std::size_t size_{};
  std::size_t index_capacity_{};
  const JitBlockLink *empty_link_{};
  JitIndirectLink *records_{};
  std::unique_ptr<std::uint32_t[]> index_;
};

static_assert(
    link_capability(JitLinkCapability::store_publication) == 1U);
static_assert(jit_link_selector_count == 10);
static_assert(valid_link_capabilities(0));
static_assert(valid_link_capabilities(1));
static_assert(valid_link_capabilities(2));
static_assert(valid_link_capabilities(3));
static_assert(valid_link_capabilities(6));
static_assert(valid_link_capabilities(7));
static_assert(valid_link_capabilities(10));
static_assert(valid_link_capabilities(11));
static_assert(valid_link_capabilities(14));
static_assert(valid_link_capabilities(15));
static_assert(link_selector_for_capabilities(0) == 0);
static_assert(link_selector_for_capabilities(15) == 9);
static_assert(link_selector_for_capabilities(4) ==
              invalid_link_selector);
static_assert(alignof(JitBlockLink) == 128);
static_assert(sizeof(JitBlockLink) == 128);
static_assert(offsetof(JitBlockLink, guest_pc) == 0);
static_assert(offsetof(JitBlockLink, instruction_count) == 8);
static_assert(offsetof(JitBlockLink, entries) == 16);
static_assert(sizeof(JitBlockLink::entries) ==
              jit_link_selector_count * sizeof(std::uintptr_t));
static_assert(sizeof(std::uintptr_t) == 8);
static_assert(offsetof(JitBlockLink, activation_epoch) == 96);
static_assert(offsetof(JitIndirectLinkEntry, guest_pc) == 0);
static_assert(offsetof(JitIndirectLinkEntry, link_entries) == 8);
static_assert(sizeof(JitIndirectLinkEntry) == 16);
static_assert(offsetof(JitIndirectLink, entries) == 0);
static_assert(offsetof(JitIndirectLink, source_pc) == 32);

} // namespace uml::cpu::arm64
