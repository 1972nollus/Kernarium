// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_compiler_families.hpp"
#include "jit_emitter.hpp"
#include "jit_linking.hpp"

namespace uml::cpu::arm64::jit {

void JitCompilerFamilies::attach_family_emitter(void *emitter,
                                                bool *native_fp,
                                                bool *native_fpmr,
                                                bool enable_gpr_cache) noexcept {
  family_emitter_ = emitter;
  family_native_fp_ = native_fp;
  family_native_fpmr_ = native_fpmr;
  gpr_cache_enabled_ = enable_gpr_cache;
}

void JitCompilerFamilies::attach_link_resolver(
    void *context, LinkResolver resolver) noexcept {
  link_context_ = context;
  link_resolver_ = resolver;
}

void JitCompilerFamilies::attach_indirect_link_resolver(
    void *context, IndirectLinkResolver resolver) noexcept {
  indirect_link_context_ = context;
  indirect_link_resolver_ = resolver;
}

void JitCompilerFamilies::begin_gpr_residency_observation(
    bool enabled) noexcept {
  gpr_residency_observation_enabled_ = enabled && gpr_cache_enabled_;
  gpr_residency_next_access_ = 0;
  gpr_residency_guests_.fill(GprResidencyGuest{});
}

void JitCompilerFamilies::observe_gpr_access(
    unsigned guest, bool stack_pointer, bool reads, bool writes) noexcept {
  if (!gpr_residency_observation_enabled_ || guest > 31U ||
      (guest == 31U && !stack_pointer) || (!reads && !writes))
    return;

  auto &observation = gpr_residency_guests_[guest];
  if (observation.first_access == GprResidencyGuest::no_first_access)
    observation.first_access = gpr_residency_next_access_;
  ++gpr_residency_next_access_;
  if (reads) {
    ++observation.reads;
    if (!observation.written)
      observation.read_before_write = true;
  }
  if (writes) {
    ++observation.writes;
    observation.written = true;
  }
}

std::size_t JitCompilerFamilies::residency_benefit(
    const GprResidencyGuest &guest) noexcept {
  if (guest.first_access == GprResidencyGuest::no_first_access)
    return 0;
  const auto fallback_accesses = guest.reads + guest.writes;
  const auto resident_accesses =
      (guest.read_before_write ? 1U : 0U) +
      (guest.written ? 1U : 0U);
  return fallback_accesses > resident_accesses
             ? fallback_accesses - resident_accesses
             : 0U;
}

JitCompilerFamilies::GprResidencyPlan
JitCompilerFamilies::finish_gpr_residency_observation() const noexcept {
  GprResidencyPlan result{};
  if (!gpr_residency_observation_enabled_)
    return result;

  std::size_t observed_guests = 0;
  for (const auto &guest : gpr_residency_guests_)
    observed_guests +=
        guest.first_access != GprResidencyGuest::no_first_access;
  if (observed_guests <= resident_gpr_hosts.size())
    return result;

  std::array<bool, 32> selected{};
  std::size_t selected_benefit = 0;
  for (std::size_t slot = 0; slot < resident_gpr_hosts.size(); ++slot) {
    unsigned best = unbound_guest;
    for (unsigned guest = 0; guest < gpr_residency_guests_.size(); ++guest) {
      const auto &candidate = gpr_residency_guests_[guest];
      if (selected[guest] ||
          candidate.first_access == GprResidencyGuest::no_first_access)
        continue;
      if (best == unbound_guest ||
          residency_benefit(candidate) >
              residency_benefit(gpr_residency_guests_[best]) ||
          (residency_benefit(candidate) ==
               residency_benefit(gpr_residency_guests_[best]) &&
           candidate.first_access <
               gpr_residency_guests_[best].first_access))
        best = guest;
    }
    if (best == unbound_guest)
      break;
    selected[best] = true;
    selected_benefit += residency_benefit(gpr_residency_guests_[best]);
    ++result.count;
  }

  if (result.count != resident_gpr_hosts.size())
    return {};

  std::size_t current_benefit = 0;
  for (const auto &slot : gpr_slots_)
    if (slot.guest != unbound_guest)
      current_benefit +=
          residency_benefit(gpr_residency_guests_[slot.guest]);
  if (selected_benefit <= current_benefit)
    return {};

  /*
   * Assign the selected identities in encounter order. This keeps the host
   * register numbering deterministic and preserves the existing first-use
   * shape among identities that survive the pressure decision.
   */
  for (std::size_t index = 0; index < result.count; ++index) {
    unsigned first = unbound_guest;
    for (unsigned guest = 0; guest < selected.size(); ++guest) {
      if (!selected[guest])
        continue;
      if (first == unbound_guest ||
          gpr_residency_guests_[guest].first_access <
              gpr_residency_guests_[first].first_access)
        first = guest;
    }
    result.guests[index] = first;
    selected[first] = false;
  }
  result.estimated_avoided_accesses =
      selected_benefit - current_benefit;
  return result;
}

void JitCompilerFamilies::apply_gpr_residency_plan(
    const GprResidencyPlan &plan) noexcept {
  if (!gpr_cache_enabled_ || plan.count != resident_gpr_hosts.size())
    return;
  std::array<bool, 32> seen{};
  for (std::size_t index = 0; index < plan.count; ++index) {
    if (plan.guests[index] >= seen.size() || seen[plan.guests[index]])
      return;
    seen[plan.guests[index]] = true;
  }
  for (std::size_t index = 0; index < plan.count; ++index) {
    gpr_slots_[index].guest = plan.guests[index];
    gpr_slots_[index].stack_pointer = plan.guests[index] == 31U;
  }
}

bool JitCompilerFamilies::gpr_slots_unbound() const noexcept {
  return gpr_cache_enabled_ &&
         std::ranges::all_of(gpr_slots_, [](const auto &slot) {
           return slot.guest == unbound_guest;
         });
}

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
void JitCompilerFamilies::attach_gpr_link_profile(
    void *context, DirectLinkProfileResolver direct_resolver,
    IndirectLinkProfileResolver indirect_resolver) noexcept {
  gpr_link_profile_context_ = context;
  direct_link_profile_resolver_ = direct_resolver;
  indirect_link_profile_resolver_ = indirect_resolver;
}

void JitCompilerFamilies::begin_gpr_link_profile_block(
    std::uint64_t pc) noexcept {
  profile_block_pc_ = pc;
  profile_gpr_read_before_write_ = 0;
  profile_gpr_written_ = 0;
}

std::uint32_t
JitCompilerFamilies::gpr_profile_read_before_write_mask() const noexcept {
  return profile_gpr_read_before_write_;
}

std::uint32_t
JitCompilerFamilies::gpr_profile_written_mask() const noexcept {
  return profile_gpr_written_;
}

std::uint32_t
JitCompilerFamilies::gpr_profile_dirty_live_out_mask() const noexcept {
  std::uint32_t result{};
  for (const auto &slot : gpr_slots_)
    if (slot.dirty &&
        (slot.guest < 31U ||
         (slot.guest == 31U && slot.stack_pointer)))
      result |= std::uint32_t{1} << slot.guest;
  return result;
}

void JitCompilerFamilies::profile_gpr_read(unsigned guest,
                                           bool stack_pointer) noexcept {
  if (guest > 31U || (guest == 31U && !stack_pointer))
    return;
  const auto bit = std::uint32_t{1} << guest;
  if ((profile_gpr_written_ & bit) == 0)
    profile_gpr_read_before_write_ |= bit;
}

void JitCompilerFamilies::profile_gpr_write(unsigned guest,
                                            bool stack_pointer) noexcept {
  if (guest > 31U || (guest == 31U && !stack_pointer))
    return;
  profile_gpr_written_ |= std::uint32_t{1} << guest;
}
#endif

#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
void JitCompilerFamilies::attach_gpr_pressure_profile(bool enabled) noexcept {
  gpr_pressure_profile_enabled_ = enabled;
}

void JitCompilerFamilies::begin_gpr_pressure_profile_block() noexcept {
  gpr_pressure_next_rank_ = 0;
  gpr_pressure_profile_ = {};
  gpr_pressure_guests_.fill(GprPressureGuest{});
}

void JitCompilerFamilies::profile_gpr_slot_allocation(
    std::size_t slots) noexcept {
  if (!gpr_pressure_profile_enabled_)
    return;
  gpr_pressure_profile_.slot_high_water =
      std::max(gpr_pressure_profile_.slot_high_water, slots);
}

JitCompilerFamilies::GprPressureGuest *
JitCompilerFamilies::find_profiled_overflow_guest(
    unsigned guest, bool stack_pointer) noexcept {
  if (!gpr_pressure_profile_enabled_ || guest > 31U ||
      (guest == 31U && !stack_pointer))
    return nullptr;
  auto &profile = gpr_pressure_guests_[guest];
  return profile.rank == GprPressureGuest::no_rank ? nullptr : &profile;
}

void JitCompilerFamilies::profile_gpr_overflow_native_access(
    unsigned guest, bool stack_pointer, bool reads_old,
    bool writes) noexcept {
  if (!gpr_pressure_profile_enabled_ || !gpr_cache_enabled_ ||
      guest > 31U ||
      (guest == 31U && !stack_pointer))
    return;
  auto &profile = gpr_pressure_guests_[guest];
  if (profile.rank == GprPressureGuest::no_rank) {
    profile.rank = static_cast<unsigned>(gpr_pressure_next_rank_++);
  } else {
    ++gpr_pressure_profile_.repeated_overflow_accesses;
  }
  ++gpr_pressure_profile_.allocation_refusals;
  if (reads_old) {
    ++profile.fallback_read_loads;
    ++gpr_pressure_profile_.fallback_read_loads;
    if (!profile.resident_value_available)
      ++profile.required_resident_accesses;
    profile.resident_value_available = true;
  }
  if (writes) {
    profile.resident_value_available = true;
    profile.dirty = true;
  }
}

void JitCompilerFamilies::profile_gpr_overflow_nonallocating_access(
    unsigned guest, bool stack_pointer, bool reads_old,
    bool writes) noexcept {
  if (!gpr_pressure_profile_enabled_ || !gpr_cache_enabled_ ||
      guest > 31U || (guest == 31U && !stack_pointer))
    return;
  auto &profile = gpr_pressure_guests_[guest];
  if (profile.rank == GprPressureGuest::no_rank) {
    const bool bank_full = std::ranges::all_of(
        gpr_slots_, [](const auto &slot) {
          return slot.guest != unbound_guest;
        });
    if (!bank_full)
      return;
    profile.rank = static_cast<unsigned>(gpr_pressure_next_rank_++);
  } else {
    ++gpr_pressure_profile_.repeated_overflow_accesses;
  }
  if (reads_old) {
    ++profile.fallback_read_loads;
    ++gpr_pressure_profile_.fallback_read_loads;
    if (!profile.resident_value_available)
      ++profile.required_resident_accesses;
    profile.resident_value_available = true;
  }
  if (writes) {
    ++profile.fallback_write_stores;
    ++gpr_pressure_profile_.fallback_write_stores;
    profile.resident_value_available = true;
    profile.dirty = true;
  }
}

void JitCompilerFamilies::profile_gpr_overflow_fallback_store(
    unsigned guest, bool stack_pointer) noexcept {
  auto *profile =
      find_profiled_overflow_guest(guest, stack_pointer);
  if (profile == nullptr)
    return;
  ++profile->fallback_write_stores;
  ++gpr_pressure_profile_.fallback_write_stores;
}

JitGprPressureProfile
JitCompilerFamilies::finish_gpr_pressure_profile_block() const noexcept {
  auto result = gpr_pressure_profile_;
  if (!gpr_pressure_profile_enabled_)
    return result;
  for (const auto &guest : gpr_pressure_guests_) {
    if (guest.rank == GprPressureGuest::no_rank)
      continue;
    const auto actual =
        guest.fallback_read_loads + guest.fallback_write_stores;
    const auto required =
        guest.required_resident_accesses + (guest.dirty ? 1U : 0U);
    const auto avoided = actual > required ? actual - required : 0U;
    for (std::size_t index = 0;
         index < jit_gpr_pressure_candidate_capacities.size(); ++index) {
      const auto extra_slots =
          jit_gpr_pressure_candidate_capacities[index] -
          resident_gpr_hosts.size();
      if (guest.rank < extra_slots)
        result.estimated_avoided_accesses[index] += avoided;
    }
  }
  return result;
}
#endif

void JitCompilerFamilies::emit(std::uint32_t instruction) {
  static_cast<Arm64Emitter *>(family_emitter_)->emit(instruction);
}

JitCompilerFamilies::Label JitCompilerFamilies::make_label() {
  return static_cast<Arm64Emitter *>(family_emitter_)->make_label();
}

void JitCompilerFamilies::bind(Label label) {
  static_cast<Arm64Emitter *>(family_emitter_)->bind(label);
}

void JitCompilerFamilies::branch_condition(Label label, unsigned condition) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->branch_condition(label, condition);
}

void JitCompilerFamilies::compare_zero(Label label, unsigned reg,
                                       bool is_64_bit, bool nonzero) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->compare_zero(label, reg, is_64_bit, nonzero);
}

void JitCompilerFamilies::test_zero(Label label, unsigned reg, unsigned bit,
                                    bool nonzero) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->test_zero(label, reg, bit, nonzero);
}

void JitCompilerFamilies::load_constant(unsigned destination,
                                        std::uint64_t value) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->load_constant(destination, value);
}

void JitCompilerFamilies::load_literal_constant(unsigned destination,
                                                std::uint64_t value) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->load_literal_constant(destination, value);
}

void JitCompilerFamilies::load_x(unsigned destination, unsigned base,
                                 std::size_t offset) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->load_x(destination, base, offset);
}

void JitCompilerFamilies::store_x(unsigned source, unsigned base,
                                  std::size_t offset) {
  static_cast<Arm64Emitter *>(family_emitter_)->store_x(source, base, offset);
}

void JitCompilerFamilies::load_q(unsigned destination, unsigned base,
                                 std::size_t offset) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  constexpr auto vectors = offsetof(arm64::CpuState, vectors);
  constexpr auto vector_bytes =
      sizeof(arm64::CpuState::vectors);
  if (gpr_cache_enabled_ && base == 15U && offset >= vectors &&
      offset < vectors + vector_bytes &&
      ((offset - vectors) % sizeof(arm64::VectorRegister)) == 0U) {
    const auto guest = static_cast<unsigned>(
        (offset - vectors) / sizeof(arm64::VectorRegister));
    const auto slot = find_vector_slot(guest);
    if (slot) {
      auto &binding = vector_slots_[*slot];
      if (!binding.loaded) {
        emitter.load_q(resident_vector_hosts[*slot], 15, offset);
        binding.loaded = true;
      }
      if (resident_vector_hosts[*slot] != destination)
        emitter.move_q(destination, resident_vector_hosts[*slot]);
      return;
    }
  }
  emitter.load_q(destination, base, offset);
}

void JitCompilerFamilies::store_q(unsigned source, unsigned base,
                                  std::size_t offset) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  constexpr auto vectors = offsetof(arm64::CpuState, vectors);
  constexpr auto vector_bytes =
      sizeof(arm64::CpuState::vectors);
  if (gpr_cache_enabled_ && base == 15U && offset >= vectors &&
      offset < vectors + vector_bytes &&
      ((offset - vectors) % sizeof(arm64::VectorRegister)) == 0U) {
    const auto guest = static_cast<unsigned>(
        (offset - vectors) / sizeof(arm64::VectorRegister));
    const auto slot = find_vector_slot(guest);
    if (slot) {
      if (resident_vector_hosts[*slot] != source)
        emitter.move_q(resident_vector_hosts[*slot], source);
      vector_slots_[*slot].loaded = true;
      vector_slots_[*slot].dirty = true;
      return;
    }
  }
  emitter.store_q(source, base, offset);
}

void JitCompilerFamilies::return_constant(Event event,
                                          std::size_t instructions) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->return_constant(event, instructions);
}

void JitCompilerFamilies::return_dynamic(std::size_t instructions) {
  static_cast<Arm64Emitter *>(family_emitter_)
      ->return_dynamic(instructions);
}

std::optional<std::size_t>
JitCompilerFamilies::find_gpr_slot(unsigned guest,
                                   bool stack_pointer) const noexcept {
  if (!gpr_cache_enabled_ || (guest == 31U && !stack_pointer))
    return std::nullopt;
  stack_pointer = guest == 31U && stack_pointer;
  for (std::size_t index = 0; index < gpr_slots_.size(); ++index) {
    const auto &slot = gpr_slots_[index];
    if (slot.guest == guest && slot.stack_pointer == stack_pointer)
      return index;
  }
  return std::nullopt;
}

std::optional<std::size_t>
JitCompilerFamilies::allocate_gpr_slot(unsigned guest,
                                       bool stack_pointer) {
  if (!gpr_cache_enabled_ || (guest == 31U && !stack_pointer))
    return std::nullopt;
  stack_pointer = guest == 31U && stack_pointer;
  for (std::size_t index = 0; index < gpr_slots_.size(); ++index) {
    auto &slot = gpr_slots_[index];
    if (slot.guest != unbound_guest)
      continue;
    slot.guest = guest;
    slot.stack_pointer = stack_pointer;
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
    profile_gpr_slot_allocation(index + 1U);
#endif
    return index;
  }
  return std::nullopt;
}

JitCompilerFamilies::NativeGpr
JitCompilerFamilies::read_native_gpr(unsigned guest, bool stack_pointer,
                                     unsigned fallback) {
  observe_gpr_access(guest, stack_pointer, /*reads=*/true,
                     /*writes=*/false);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  profile_gpr_read(guest, stack_pointer);
#endif
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  if (guest == 31U && !stack_pointer)
    return {31U, true};
  auto slot = find_gpr_slot(guest, stack_pointer);
  if (!slot)
    slot = allocate_gpr_slot(guest, stack_pointer);
  if (slot) {
    auto &binding = gpr_slots_[*slot];
    if (!binding.loaded) {
      emitter.load_x(resident_gpr_hosts[*slot], 15,
                     register_offset(guest, stack_pointer));
      binding.loaded = true;
    }
    return {resident_gpr_hosts[*slot], true};
  }
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
  profile_gpr_overflow_native_access(guest, stack_pointer,
                                     /*reads_old=*/true,
                                     /*writes=*/false);
#endif
  emitter.load_x(fallback, 15, register_offset(guest, stack_pointer));
  return {fallback, false};
}

JitCompilerFamilies::NativeGpr
JitCompilerFamilies::write_native_gpr(unsigned guest, bool stack_pointer,
                                      bool reads_old, unsigned fallback) {
  observe_gpr_access(guest, stack_pointer, reads_old, /*writes=*/true);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  if (reads_old)
    profile_gpr_read(guest, stack_pointer);
#endif
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  if (guest == 31U && !stack_pointer)
    return {31U, true};
  auto slot = find_gpr_slot(guest, stack_pointer);
  if (!slot)
    slot = allocate_gpr_slot(guest, stack_pointer);
  if (slot) {
    auto &binding = gpr_slots_[*slot];
    if (reads_old && !binding.loaded)
      emitter.load_x(resident_gpr_hosts[*slot], 15,
                     register_offset(guest, stack_pointer));
    binding.loaded = true;
    binding.dirty = true;
    return {resident_gpr_hosts[*slot], true};
  }
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
  profile_gpr_overflow_native_access(guest, stack_pointer, reads_old,
                                     /*writes=*/true);
#endif
  if (reads_old)
    emitter.load_x(fallback, 15, register_offset(guest, stack_pointer));
  return {fallback, false};
}

void JitCompilerFamilies::commit_native_gpr(NativeGpr value, unsigned guest,
                                            bool stack_pointer) {
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  profile_gpr_write(guest, stack_pointer);
#endif
  if (guest == 31U && !stack_pointer)
    return;
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
  if (!value.resident)
    profile_gpr_overflow_fallback_store(guest, stack_pointer);
#endif
  if (!value.resident)
    static_cast<Arm64Emitter *>(family_emitter_)
        ->store_x(value.host, 15, register_offset(guest, stack_pointer));
}

std::optional<std::size_t>
JitCompilerFamilies::find_vector_slot(unsigned guest) const noexcept {
  if (!gpr_cache_enabled_)
    return std::nullopt;
  for (std::size_t index = 0; index < vector_slots_.size(); ++index)
    if (vector_slots_[index].guest == guest)
      return index;
  return std::nullopt;
}

std::optional<unsigned> JitCompilerFamilies::constrained_vector_source(
    std::uint32_t instruction) const noexcept {
  if (!floating_or_vector_primary_class(instruction))
    return std::nullopt;

  const auto &host = host_capabilities();

  const auto half_long =
      arm64::detail::decode_half_fused_long(instruction);
  if (half_long.recognized && half_long.by_element &&
      host.has(ArmFeature::fhm) && host.has(ArmFeature::afp))
    return (instruction >> 16U) & 15U;

  const auto bfloat_long =
      arm64::detail::decode_bfloat_multiply_long(instruction);
  if (bfloat_long.recognized && bfloat_long.by_element &&
      host.has(ArmFeature::bf16))
    return (instruction >> 16U) & 15U;

  const auto fp8 = arm64::detail::decode_fp8(instruction);
  if (fp8.recognized && fp8.valid && fp8.by_element) {
    using Operation = arm64::detail::Fp8Operation;
    if (fp8.operation == Operation::dot_half &&
        host.has(ArmFeature::fp8dot2))
      return fp8.second;
    if ((fp8.operation == Operation::multiply_half ||
         fp8.operation == Operation::multiply_single) &&
        host.has(ArmFeature::fp8fma))
      return fp8.second;
  }

  const unsigned size = (instruction >> 22U) & 3U;
  if (size == 1U && valid_simd_integer_indexed(instruction) &&
      host.advanced_simd) {
    const bool unsigned_group =
        (instruction & 0x2000'0000U) != 0U;
    const unsigned operation = (instruction >> 12U) & 15U;
    const bool requires_rdm =
        unsigned_group && (operation == 13U || operation == 15U);
    if (!requires_rdm || host.has(ArmFeature::rdm))
      return (instruction >> 16U) & 15U;
  }

  if (size == 0U && valid_simd_floating_indexed(instruction) &&
      host.floating_point && host.advanced_simd && host.fp16)
    return (instruction >> 16U) & 15U;

  return std::nullopt;
}

void JitCompilerFamilies::pin_constrained_vector(unsigned guest) noexcept {
  if (!gpr_cache_enabled_ || guest >= 32U)
    return;
  if (find_vector_slot(guest))
    return;
  for (std::size_t index = 0; index < pinned_vector_slot_count; ++index) {
    auto &slot = vector_slots_[index];
    if (slot.guest != unbound_guest)
      continue;
    slot.guest = guest;
    return;
  }
}

std::optional<JitCompilerFamilies::ConsecutiveVectorList>
JitCompilerFamilies::consecutive_vector_list(
    std::uint32_t instruction) const noexcept {
  const auto &host = host_capabilities();
  if (!host.advanced_simd)
    return std::nullopt;

  if ((instruction & 0xff20'8c00U) == 0x4e00'0000U &&
      (instruction & 0x00c0'0000U) != 0U) {
    if (!host.has(ArmFeature::lut))
      return std::nullopt;
    const unsigned op2 = (instruction >> 22U) & 3U;
    const unsigned len = (instruction >> 13U) & 3U;
    const unsigned op = (instruction >> 12U) & 1U;
    const bool valid =
        op2 == 3U || (op2 == 2U && op != 0U) ||
        (op2 == 1U && (op != 0U || (len & 1U) != 0U));
    if (!valid || op2 != 1U || op == 0U)
      return std::nullopt;
    return ConsecutiveVectorList{
        .first = (instruction >> 5U) & 31U,
        .count = 2U,
    };
  }

  if ((instruction & 0xbfe0'8c00U) != 0x0e00'0000U)
    return std::nullopt;
  const unsigned count = ((instruction >> 13U) & 3U) + 1U;
  if (count == 1U)
    return std::nullopt;
  return ConsecutiveVectorList{
      .first = (instruction >> 5U) & 31U,
      .count = count,
  };
}

bool JitCompilerFamilies::pin_consecutive_vector_list(
    unsigned first, unsigned count) noexcept {
  if (!gpr_cache_enabled_ || first >= 32U || count < 2U ||
      count > 4U || count > vector_slots_.size() - pinned_vector_slot_count)
    return false;

  /*
   * q6/q7 remain dedicated to the more restrictive indexed encodings.
   * Search q16-q31 from the high end so ordinary first-use allocation keeps
   * its existing q16-up order. Every compatible binding is resolved here;
   * the generated table operation contains no staging decision or branch.
   */
  const auto first_start = vector_slots_.size() - count;
  for (std::size_t start = first_start + 1U;
       start-- > pinned_vector_slot_count;) {
    bool compatible = true;
    for (unsigned member = 0; member < count; ++member) {
      const unsigned guest = (first + member) & 31U;
      const auto existing = find_vector_slot(guest);
      if ((existing && *existing != start + member) ||
          (vector_slots_[start + member].guest != unbound_guest &&
           vector_slots_[start + member].guest != guest)) {
        compatible = false;
        break;
      }
      if (member != 0U &&
          resident_vector_hosts[start + member] !=
              resident_vector_hosts[start] + member) {
        compatible = false;
        break;
      }
    }
    if (!compatible)
      continue;
    for (unsigned member = 0; member < count; ++member)
      vector_slots_[start + member].guest = (first + member) & 31U;
    return true;
  }
  return false;
}

std::optional<std::size_t>
JitCompilerFamilies::allocate_vector_slot(unsigned guest) {
  if (!gpr_cache_enabled_)
    return std::nullopt;
  /*
   * Keep the unrestricted low-pressure shape stable by consuming q16-q31
   * first.  q6/q7 are prebound to constrained identities before emission;
   * any low slot left unbound after that planning pass is ordinary resident
   * capacity once the high bank is full.
   */
  for (std::size_t index = pinned_vector_slot_count;
       index < vector_slots_.size(); ++index) {
    auto &slot = vector_slots_[index];
    if (slot.guest != unbound_guest)
      continue;
    slot.guest = guest;
    return index;
  }
  for (std::size_t index = 0; index < pinned_vector_slot_count; ++index) {
    auto &slot = vector_slots_[index];
    if (slot.guest != unbound_guest)
      continue;
    slot.guest = guest;
    return index;
  }
  return std::nullopt;
}

JitCompilerFamilies::NativeVector
JitCompilerFamilies::read_native_vector(unsigned guest,
                                        unsigned fallback) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  auto slot = find_vector_slot(guest);
  if (!slot)
    slot = allocate_vector_slot(guest);
  const auto offset =
      offsetof(arm64::CpuState, vectors) +
      guest * sizeof(arm64::VectorRegister);
  if (slot) {
    auto &binding = vector_slots_[*slot];
    if (!binding.loaded) {
      emitter.load_q(resident_vector_hosts[*slot], 15, offset);
      binding.loaded = true;
    }
    return {resident_vector_hosts[*slot], true};
  }
  emitter.load_q(fallback, 15, offset);
  return {fallback, false};
}

JitCompilerFamilies::NativeVector
JitCompilerFamilies::read_constrained_native_vector(unsigned guest,
                                                    unsigned fallback) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  const auto offset =
      offsetof(arm64::CpuState, vectors) +
      guest * sizeof(arm64::VectorRegister);
  const auto slot = find_vector_slot(guest);
  if (slot && *slot < pinned_vector_slot_count) {
    auto &binding = vector_slots_[*slot];
    if (!binding.loaded) {
      emitter.load_q(resident_vector_hosts[*slot], 15, offset);
      binding.loaded = true;
    }
    return {resident_vector_hosts[*slot], true};
  }

  /*
   * A third constrained identity deliberately retains the established v2
   * fallback rather than shortening the block. load_q() is cache-aware, so
   * an earlier resident write is copied from its live host binding instead
   * of reloading stale CpuState.
   */
  load_q(fallback, 15, offset);
  return {fallback, false};
}

JitCompilerFamilies::NativeVector
JitCompilerFamilies::write_native_vector(unsigned guest, bool reads_old,
                                         unsigned fallback) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  auto slot = find_vector_slot(guest);
  if (!slot)
    slot = allocate_vector_slot(guest);
  const auto offset =
      offsetof(arm64::CpuState, vectors) +
      guest * sizeof(arm64::VectorRegister);
  if (slot) {
    auto &binding = vector_slots_[*slot];
    if (reads_old && !binding.loaded)
      emitter.load_q(resident_vector_hosts[*slot], 15, offset);
    binding.loaded = true;
    binding.dirty = true;
    return {resident_vector_hosts[*slot], true};
  }
  if (reads_old)
    emitter.load_q(fallback, 15, offset);
  return {fallback, false};
}

void JitCompilerFamilies::commit_native_vector(NativeVector value,
                                               unsigned guest) {
  if (value.resident)
    return;
  static_cast<Arm64Emitter *>(family_emitter_)
      ->store_q(value.host, 15,
                offsetof(arm64::CpuState, vectors) +
                    guest * sizeof(arm64::VectorRegister));
}

void JitCompilerFamilies::load_gpr(unsigned host, unsigned guest,
                                   bool stack_pointer) {
  observe_gpr_access(guest, stack_pointer, /*reads=*/true,
                     /*writes=*/false);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  profile_gpr_read(guest, stack_pointer);
#endif
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  if (guest == 31U && !stack_pointer) {
    emitter.move_x(host, 31);
    return;
  }
  const auto slot = find_gpr_slot(guest, stack_pointer);
  if (slot) {
    if (!gpr_slots_[*slot].loaded) {
      emitter.load_x(resident_gpr_hosts[*slot], 15,
                     register_offset(guest, stack_pointer));
      gpr_slots_[*slot].loaded = true;
    }
    if (resident_gpr_hosts[*slot] != host)
      emitter.move_x(host, resident_gpr_hosts[*slot]);
    return;
  }
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
  profile_gpr_overflow_nonallocating_access(
      guest, stack_pointer, /*reads_old=*/true, /*writes=*/false);
#endif
  emitter.load_x(host, 15, register_offset(guest, stack_pointer));
}

void JitCompilerFamilies::store_gpr(unsigned host, unsigned guest,
                                    bool stack_pointer) {
  observe_gpr_access(guest, stack_pointer, /*reads=*/false,
                     /*writes=*/true);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  profile_gpr_write(guest, stack_pointer);
#endif
  if (guest == 31U && !stack_pointer)
    return;
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  const auto slot = find_gpr_slot(guest, stack_pointer);
  if (slot) {
    if (resident_gpr_hosts[*slot] != host)
      emitter.move_x(resident_gpr_hosts[*slot], host);
    gpr_slots_[*slot].loaded = true;
    gpr_slots_[*slot].dirty = true;
    return;
  }
#if defined(DARWIN_UML_JIT_GPR_PRESSURE_PROFILE)
  profile_gpr_overflow_nonallocating_access(
      guest, stack_pointer, /*reads_old=*/false, /*writes=*/true);
#endif
  emitter.store_x(host, 15, register_offset(guest, stack_pointer));
}

std::uint16_t JitCompilerFamilies::dirty_gpr_mask() const noexcept {
  std::uint16_t mask = 0;
  for (std::size_t index = 0; index < gpr_slots_.size(); ++index)
    if (gpr_slots_[index].dirty)
      mask |= static_cast<std::uint16_t>(1U << index);
  return mask;
}

std::uint32_t JitCompilerFamilies::dirty_vector_mask() const noexcept {
  std::uint32_t mask = 0;
  for (std::size_t index = 0; index < vector_slots_.size(); ++index)
    if (vector_slots_[index].dirty)
      mask |= std::uint32_t{1} << index;
  return mask;
}

void JitCompilerFamilies::flush_gprs(std::uint16_t mask) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  for (std::size_t index = 0; index < gpr_slots_.size(); ++index) {
    const auto &slot = gpr_slots_[index];
    if ((mask & (1U << index)) == 0U || slot.guest == unbound_guest)
      continue;
    emitter.store_x(resident_gpr_hosts[index], 15,
                    register_offset(slot.guest, slot.stack_pointer));
  }
}

void JitCompilerFamilies::flush_vectors(std::uint32_t mask) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  for (std::size_t index = 0; index < vector_slots_.size(); ++index) {
    const auto &slot = vector_slots_[index];
    if ((mask & (1U << index)) == 0U || slot.guest == unbound_guest)
      continue;
    emitter.store_q(
        resident_vector_hosts[index], 15,
        offsetof(arm64::CpuState, vectors) +
            slot.guest * sizeof(arm64::VectorRegister));
  }
}

void JitCompilerFamilies::set_exit_gpr_mask(std::uint16_t mask) noexcept {
  exit_gpr_mask_ = mask;
  exit_gpr_mask_set_ = true;
}

void JitCompilerFamilies::set_exit_vector_mask(
    std::uint32_t mask) noexcept {
  exit_vector_mask_ = mask;
  exit_vector_mask_set_ = true;
}

void JitCompilerFamilies::flush_exit_gprs() {
  const auto gpr_mask =
      exit_gpr_mask_set_ ? exit_gpr_mask_ : dirty_gpr_mask();
  const auto vector_mask =
      exit_vector_mask_set_ ? exit_vector_mask_ : dirty_vector_mask();
  flush_gprs(gpr_mask);
  flush_vectors(vector_mask);
  /*
   * Exit code can be emitted on one arm of a generated branch.  Publish a
   * dirty host NZCV for that arm, but deliberately retain the translation
   * state: the other arm did not execute this store and may continue using
   * the live guest flags.
  */
  flush_flags_for_exit();
  exit_gpr_mask_set_ = false;
  exit_vector_mask_set_ = false;
}

void JitCompilerFamilies::spill_gprs_for_returning_call() {
  flush_gprs(dirty_gpr_mask());
  flush_vectors(dirty_vector_mask());
  spill_flags_for_internal_clobber();
}

void JitCompilerFamilies::reload_gprs_after_call() {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  for (std::size_t index = 0; index < gpr_slots_.size(); ++index) {
    const auto &slot = gpr_slots_[index];
    if (slot.guest == unbound_guest)
      continue;
    emitter.load_x(resident_gpr_hosts[index], 15,
                   register_offset(slot.guest, slot.stack_pointer));
  }
  for (std::size_t index = 0; index < vector_slots_.size(); ++index) {
    const auto &slot = vector_slots_[index];
    if (slot.guest == unbound_guest || !slot.loaded)
      continue;
    emitter.load_q(
        resident_vector_hosts[index], 15,
        offsetof(arm64::CpuState, vectors) +
            slot.guest * sizeof(arm64::VectorRegister));
  }
}

void JitCompilerFamilies::spill_gprs_for_terminal_call() {
  const auto gpr_mask =
      exit_gpr_mask_set_ ? exit_gpr_mask_ : dirty_gpr_mask();
  const auto vector_mask =
      exit_vector_mask_set_ ? exit_vector_mask_ : dirty_vector_mask();
  flush_gprs(gpr_mask);
  flush_vectors(vector_mask);
  spill_flags_for_internal_clobber();
  /*
   * The return emitter still invokes the centralized exit hook. Leave an
   * explicit empty mask so it cannot flush bindings allocated on a later hot
   * path into an earlier cold exit.
   */
  exit_gpr_mask_ = 0;
  exit_gpr_mask_set_ = true;
  exit_vector_mask_ = 0;
  exit_vector_mask_set_ = true;
}

void JitCompilerFamilies::flush_and_invalidate_gprs() {
  flush_gprs(dirty_gpr_mask());
  /*
   * Keep every slot's architectural identity stable for cold exits recorded
   * before this point. The constrained native sequence may overwrite the host
   * value, so subsequent hot-path users must reload it from the state just
   * published above.
   */
  for (auto &slot : gpr_slots_) {
    slot.loaded = false;
    slot.dirty = false;
  }
}

void JitCompilerFamilies::store_pc(std::uint64_t pc) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  emitter.load_literal_constant(8, pc);
  emitter.store_x(8, 15, offsetof(arm64::CpuState, pc));
}

void JitCompilerFamilies::load_flags() {
  if (guest_flags_state_ != GuestFlagsState::memory)
    return;
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  emitter.load_w(11, 15, offsetof(arm64::CpuState, nzcv));
  emitter.emit(0xd51b'420bU); // msr nzcv, x11
  guest_flags_state_ = GuestFlagsState::synchronized;
}

void JitCompilerFamilies::store_flags() {
  /* A native guest instruction has just produced the complete guest NZCV. */
  guest_flags_state_ = GuestFlagsState::host_dirty;
}

void JitCompilerFamilies::flush_flags_for_exit() {
  if (guest_flags_state_ != GuestFlagsState::host_dirty)
    return;
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  emitter.emit(0xd53b'420bU); // mrs x11, nzcv
  emitter.store_w(11, 15, offsetof(arm64::CpuState, nzcv));
}

void JitCompilerFamilies::synchronize_flags() {
  flush_flags_for_exit();
  if (guest_flags_state_ == GuestFlagsState::host_dirty)
    guest_flags_state_ = GuestFlagsState::synchronized;
}

void JitCompilerFamilies::spill_flags_for_internal_clobber() {
  flush_flags_for_exit();
  guest_flags_state_ = GuestFlagsState::memory;
}

void JitCompilerFamilies::assume_flags_in_memory() noexcept {
  guest_flags_state_ = GuestFlagsState::memory;
}

JitCompilerFamilies::GuestFlagsState
JitCompilerFamilies::snapshot_flags() const noexcept {
  return guest_flags_state_;
}

void JitCompilerFamilies::publish_cold_flags(GuestFlagsState state) {
  if (state != GuestFlagsState::host_dirty)
    return;
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  emitter.emit(0xd53b'420bU); // mrs x11, nzcv
  emitter.store_w(11, 15, offsetof(arm64::CpuState, nzcv));
}

void JitCompilerFamilies::read_flags(unsigned destination) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  if (guest_flags_state_ == GuestFlagsState::memory) {
    emitter.load_w(destination, 15, offsetof(arm64::CpuState, nzcv));
    return;
  }
  emitter.emit(0xd53b'4200U | (destination & 31U)); // mrs Xd, nzcv
}

void JitCompilerFamilies::mark_native_fp() noexcept {
  *family_native_fp_ = true;
}

void JitCompilerFamilies::mark_native_fpmr() noexcept {
  *family_native_fpmr_ = true;
}

void JitCompilerFamilies::emit_linked_exit(std::uint64_t target,
                                           std::size_t count) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  std::uint64_t *profile_counter{};
  const auto profile_dirty_live_out = gpr_profile_dirty_live_out_mask();
#endif
  flush_exit_gprs();
  /*
   * Unlike an exit emitted on only one branch arm, every path entering this
   * linked-exit sequence executes the flush above.  CpuState is therefore
   * canonical before the publication/fairness comparisons take ownership of
   * host NZCV.
   */
  assume_flags_in_memory();
  set_exit_gpr_mask(0);
  set_exit_vector_mask(0);
  const auto *link =
      link_resolver_ == nullptr
          ? nullptr
          : static_cast<const JitBlockLink *>(
                link_resolver_(link_context_, target));
  if (link == nullptr) {
    store_pc(target);
    emitter.return_constant(Event::continued, count);
    return;
  }
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  if (direct_link_profile_resolver_ != nullptr) {
    const auto source_pc =
        profile_block_pc_ +
        (count == 0 ? 0 : (count - 1) * sizeof(std::uint32_t));
    profile_counter = direct_link_profile_resolver_(
        gpr_link_profile_context_, link, source_pc, profile_dirty_live_out);
  }
#endif

  const auto fallback = emitter.make_label();
  emitter.load_stable_pointer(
      12, reinterpret_cast<std::uintptr_t>(link->entries.data()));
  if (emitter.persistent_linked_block_abi()) {
    /*
     * Subtract the completed source while the descriptor pointer load is in
     * flight. Both a missing capability and a full fairness window take the
     * shared cold undo below, so only successful links commit the decrement.
     * x21 is initialized once from the host-owned capability selector; guest
     * data never contributes to the indexed native branch target.
     */
    emitter.subtract_immediate(20, 20, static_cast<unsigned>(count));
    emitter.load_x_indexed_8(13, 12, 21);
  } else {
    emitter.load_x(11, 31, 32); // Saved JitChainState *.
    emitter.load_b(9, 11, offsetof(JitChainState, link_selector));
    emitter.load_x_indexed_8(13, 12, 9);
  }
  emitter.load_pair_x(
      16, 9, 12,
      -static_cast<std::ptrdiff_t>(offsetof(JitBlockLink, entries)));
  emitter.compare_zero(fallback, 13, true, false);

  if (emitter.persistent_linked_block_abi()) {
    emitter.compare_register(20, 9);
  } else {
    emitter.load_x(10, 11, offsetof(JitChainState, remaining_instructions));
    emitter.subtract_immediate(10, 10, static_cast<unsigned>(count));
    emitter.compare_register(10, 9);
  }
  emitter.branch_condition(fallback, 3); // LO

  if (!emitter.persistent_linked_block_abi())
    emitter.store_x(10, 11, offsetof(JitChainState, remaining_instructions));
  /*
   * x15/x14 still hold CpuState/TLB, and the outer frame owns all chain state
   * for both generated ABI variants. Enter the successor after its external
   * prologue so a successful link executes no argument reload and creates no
   * extra native frame. Every successor exit therefore returns through the
   * one frame established by the initial C++ entry.
   */
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  if (profile_counter != nullptr) {
    emitter.load_stable_pointer(
        9, reinterpret_cast<std::uintptr_t>(profile_counter));
    emitter.load_x(8, 9, 0);
    emitter.add_immediate(8, 8, 1);
    emitter.store_x(8, 9, 0);
  }
#endif
  emitter.branch_register(13);

  emitter.bind(fallback);
  if (emitter.persistent_linked_block_abi())
    emitter.add_immediate(20, 20, static_cast<unsigned>(count));
  store_pc(target);
  emitter.return_constant(Event::continued, count);
}

void JitCompilerFamilies::emit_indirect_linked_exit(
    unsigned target_host, bool writes_link, std::uint64_t source_pc,
    std::size_t count) {
  auto &emitter = *static_cast<Arm64Emitter *>(family_emitter_);
  /*
   * Preserve the architectural target before BLR can replace a resident X30
   * binding. x8 is executor-owned scratch and is never a native branch
   * operand on this path.
   */
  if (target_host != 8U)
    emitter.move_x(8, target_host);
  if (writes_link) {
    const auto link_register =
        write_native_gpr(30, false, false, 9);
    emitter.load_literal_constant(link_register.host,
                                  source_pc + sizeof(std::uint32_t));
    commit_native_gpr(link_register, 30, false);
  }

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  const auto profile_dirty_live_out = gpr_profile_dirty_live_out_mask();
#endif
  flush_exit_gprs();
  /*
   * CpuState is canonical before host-only comparisons take ownership of
   * NZCV. Internal successors reload guest flags on demand.
   */
  assume_flags_in_memory();
  set_exit_gpr_mask(0);
  set_exit_vector_mask(0);

  const auto *site =
      indirect_link_resolver_ == nullptr
          ? nullptr
          : static_cast<const JitIndirectLink *>(
                indirect_link_resolver_(indirect_link_context_, source_pc));
  if (site == nullptr) {
    emitter.store_x(8, 15, offsetof(CpuState, pc));
    emitter.return_constant(Event::continued, count);
    return;
  }

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  JitIndirectLinkProfileCounters profile_counters{};
  if (indirect_link_profile_resolver_ != nullptr)
    profile_counters = indirect_link_profile_resolver_(
        gpr_link_profile_context_, site, source_pc, profile_dirty_live_out);
  const bool profile_link =
      profile_counters.first != nullptr && profile_counters.second != nullptr;
#endif

  const auto matched = emitter.make_label();
  const auto miss = emitter.make_label();
  const auto fallback = emitter.make_label();
  const auto undo_and_return_to_cpp = emitter.make_label();
  emitter.load_stable_pointer(13, reinterpret_cast<std::uintptr_t>(site));
  /*
   * Hide the independent budget update under the dependent PIC descriptor
   * load. Every miss or rejected matched entry rejoins at the one cold undo.
   */
  if (emitter.persistent_linked_block_abi())
    emitter.subtract_immediate(20, 20, static_cast<unsigned>(count));
  const auto load_entry = [&](std::size_t offset) {
    /* ldp x9, x12, [x13, #offset]: {guest target, host entry table}. */
    emitter.load_pair_x(9, 12, 13,
                        static_cast<std::ptrdiff_t>(offset));
  };
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  if (profile_link) {
    const auto first_matched = emitter.make_label();
    load_entry(offsetof(JitIndirectLink, entries));
    emitter.compare_register(8, 9);
    emitter.branch_condition(first_matched, 0); // EQ
    load_entry(offsetof(JitIndirectLink, entries) +
               sizeof(JitIndirectLinkEntry));
    emitter.compare_register(8, 9);
    emitter.branch_condition(miss, 1); // NE
    emitter.load_stable_pointer(
        17, reinterpret_cast<std::uintptr_t>(profile_counters.second));
    emitter.branch(matched);
    emitter.bind(first_matched);
    emitter.load_stable_pointer(
        17, reinterpret_cast<std::uintptr_t>(profile_counters.first));
    emitter.bind(matched);
  } else {
#endif
  load_entry(offsetof(JitIndirectLink, entries));
  emitter.compare_register(8, 9);
  emitter.branch_condition(matched, 0); // EQ
  load_entry(offsetof(JitIndirectLink, entries) +
             sizeof(JitIndirectLinkEntry));
  emitter.compare_register(8, 9);
  emitter.branch_condition(miss, 1); // NE
  emitter.bind(matched);
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  }
#endif

  /*
   * x12 is a stable host entry table selected by a guest-target equality
   * test. Only its host-published internal entry can reach BR below.
   */
  if (emitter.persistent_linked_block_abi()) {
    emitter.load_x_indexed_8(13, 12, 21);
  } else {
    emitter.load_x(11, 31, 32);
    emitter.load_b(9, 11, offsetof(JitChainState, link_selector));
    emitter.load_x_indexed_8(13, 12, 9);
  }
  emitter.load_pair_x(
      16, 9, 12,
      -static_cast<std::ptrdiff_t>(offsetof(JitBlockLink, entries)));
  emitter.compare_zero(fallback, 13, true, false);

  if (emitter.persistent_linked_block_abi()) {
    emitter.compare_register(20, 9);
  } else {
    emitter.load_x(10, 11, offsetof(JitChainState, remaining_instructions));
    emitter.subtract_immediate(10, 10, static_cast<unsigned>(count));
    emitter.compare_register(10, 9);
  }
  emitter.branch_condition(fallback, 3); // LO

  if (!emitter.persistent_linked_block_abi())
    emitter.store_x(10, 11, offsetof(JitChainState, remaining_instructions));
#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
  if (profile_link) {
    emitter.load_x(9, 17, 0);
    emitter.add_immediate(9, 9, 1);
    emitter.store_x(9, 17, 0);
  }
#endif
  emitter.branch_register(13);

  emitter.bind(miss);
  emitter.store_x(8, 15, offsetof(CpuState, pc));
  const unsigned miss_chain_state =
      emitter.persistent_linked_block_abi() ? 19U : 11U;
  if (!emitter.persistent_linked_block_abi())
    emitter.load_x(11, 31, 32);
  emitter.store_x(13, miss_chain_state,
                  offsetof(JitChainState, indirect_miss));
  emitter.load_literal_constant(9, source_pc);
  emitter.store_x(9, miss_chain_state,
                  offsetof(JitChainState, indirect_source_pc));
  emitter.branch(undo_and_return_to_cpp);

  emitter.bind(fallback);
  emitter.store_x(16, 15, offsetof(CpuState, pc));
  emitter.bind(undo_and_return_to_cpp);
  if (emitter.persistent_linked_block_abi())
    emitter.add_immediate(20, 20, static_cast<unsigned>(count));
  emitter.return_constant(Event::continued, count);
}

} // namespace uml::cpu::arm64::jit
