// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

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

struct alignas(64) JitBlockLink final {
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
   * All accepted setup combinations enter the same native address. A compact
   * bit mask says which of the ten host-owned selectors may use it, avoiding
   * ten duplicate pointers in every arena-adjacent target anchor.
   */
  std::uint64_t guest_pc{};
  std::uint64_t instruction_count{};
  std::uintptr_t internal_entry{};
  std::uint64_t activation_epoch{};
  JitBlockLink *next_target{};
  std::uint32_t allowed_selectors{};
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  std::uint32_t profile_read_before_write{};
  bool profile_mask_valid{};
#endif
};

struct JitBlockLinkActivation final {
  std::size_t instruction_count{};
  std::uintptr_t internal_entry{};
  std::uint32_t allowed_selectors{};
  std::uint64_t epoch{};
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  std::uint32_t profile_read_before_write{};
  bool profile_mask_valid{};
#endif
};

struct JitIndirectLinkEntry final {
  std::uint64_t guest_pc{};
  const JitBlockLink *link{};
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

/* Generated code embeds pointers to records stored immediately after the first
 * arena block that names a target PC. This table is only a fixed reverse
 * index: target PC -> its one arena-owned activation anchor. Later blocks use
 * that same anchor instead of duplicating mutable state per source. It never
 * owns or moves a record.
 */
class JitBlockLinkTable final {
public:
  struct Insertion final {
    JitBlockLink *previous{};
    bool inserted{};
  };

  explicit JitBlockLinkTable(std::size_t capacity);

  JitBlockLinkTable(const JitBlockLinkTable &) = delete;
  JitBlockLinkTable &operator=(const JitBlockLinkTable &) = delete;

  /* The caller owns record and writes previous into record.next_target while
   * the arena is writable. A duplicate PC returns the existing anchor without
   * inserting record. Publishing the bucket first is safe because one
   * executor is used by only one host thread at a time.
   */
  [[nodiscard]] Insertion insert(JitBlockLink &record) noexcept;
  [[nodiscard]] JitBlockLink *find(std::uint64_t pc) const noexcept;
  void clear() noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  /* Only fixed bucket heads are allocated here. Record bytes are charged as
   * part of the arena that stores code and its link footer together.
   */
  [[nodiscard]] static std::size_t
  allocation_bytes(std::size_t capacity) noexcept;

private:
  std::size_t capacity_{};
  std::size_t size_{};
  std::size_t index_capacity_{};
  std::unique_ptr<JitBlockLink *[]> index_;
};

/* GPR-link profiling resolves targets before arena publication so it can
 * attach stable counter ownership to the compilation. Keep that opt-in
 * diagnostic storage separate from the production arena-adjacent records.
 */
class JitProfileBlockLinkTable final {
public:
  explicit JitProfileBlockLinkTable(std::size_t capacity);
  ~JitProfileBlockLinkTable();

  JitProfileBlockLinkTable(const JitProfileBlockLinkTable &) = delete;
  JitProfileBlockLinkTable &operator=(const JitProfileBlockLinkTable &) =
      delete;

  [[nodiscard]] JitBlockLink *find_or_create(std::uint64_t pc);
  void clear() noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

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
 * arena-adjacent JitBlockLink records, never to native code supplied by the
 * guest.
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
static_assert(alignof(JitBlockLink) == 64);
static_assert(sizeof(JitBlockLink) == 64);
static_assert(offsetof(JitBlockLink, guest_pc) == 0);
static_assert(offsetof(JitBlockLink, instruction_count) == 8);
static_assert(offsetof(JitBlockLink, internal_entry) == 16);
static_assert(sizeof(std::uintptr_t) == 8);
static_assert(offsetof(JitBlockLink, activation_epoch) == 24);
static_assert(offsetof(JitBlockLink, next_target) == 32);
static_assert(offsetof(JitBlockLink, allowed_selectors) == 40);
static_assert(offsetof(JitIndirectLinkEntry, guest_pc) == 0);
static_assert(offsetof(JitIndirectLinkEntry, link) == 8);
static_assert(sizeof(JitIndirectLinkEntry) == 16);
static_assert(offsetof(JitIndirectLink, entries) == 0);
static_assert(offsetof(JitIndirectLink, source_pc) == 32);

} // namespace uml::cpu::arm64
