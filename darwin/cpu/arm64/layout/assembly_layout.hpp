// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/layout/assembly_offsets.h"
#include "memory/tlb.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64::layout {

inline constexpr unsigned version = UML_ARM64_ASSEMBLY_LAYOUT_VERSION;

static_assert(offsetof(CpuState, registers) == UML_GADGET_CPU_X0);
static_assert(offsetof(CpuState, sp) == UML_GADGET_CPU_SP);
static_assert(offsetof(CpuState, pc) == UML_GADGET_CPU_PC);
static_assert(offsetof(CpuState, nzcv) == UML_GADGET_CPU_NZCV);
static_assert(offsetof(CpuState, vectors) == UML_GADGET_CPU_V0);
static_assert(offsetof(CpuState, fpcr) == UML_GADGET_CPU_FPCR);
static_assert(offsetof(CpuState, fpsr) == UML_GADGET_CPU_FPSR);
static_assert(offsetof(CpuState, tls) == UML_GADGET_CPU_TLS);
static_assert(offsetof(CpuState, mte_enabled) == UML_GADGET_CPU_MTE_ENABLED);
static_assert(offsetof(CpuState, tbi_enabled) == UML_GADGET_CPU_TBI_ENABLED);
static_assert(offsetof(CpuState, mte_exclude) == UML_GADGET_CPU_MTE_EXCLUDE);
static_assert(offsetof(CpuState, mte_tag_choices) == UML_GADGET_CPU_MTE_TAG_CHOICES);
static_assert(offsetof(CpuState, mte_random_tags) == UML_GADGET_CPU_MTE_RANDOM_TAGS);
static_assert(offsetof(CpuState, mte_seed) == UML_GADGET_CPU_MTE_SEED);
static_assert(offsetof(CpuState, mte_tag) == UML_GADGET_CPU_MTE_TAG);
static_assert(offsetof(CpuState, mte_random_state) == UML_GADGET_CPU_MTE_RANDOM_STATE);
static_assert(offsetof(CpuState, cpa_enabled) == UML_GADGET_CPU_CPA_ENABLED);
static_assert(offsetof(CpuState, cpa_multiply_enabled) == UML_GADGET_CPU_CPA_MULTIPLY_ENABLED);
static_assert(offsetof(CpuState, accdata) == UML_GADGET_CPU_ACCDATA);
static_assert(offsetof(CpuState, pstate_ssbs) == UML_GADGET_CPU_PSTATE_SSBS);
static_assert(offsetof(CpuState, pstate_dit) == UML_GADGET_CPU_PSTATE_DIT);
static_assert(offsetof(CpuState, pstate_tco) == UML_GADGET_CPU_PSTATE_TCO);
static_assert(offsetof(CpuState, pstate_sm) == UML_GADGET_CPU_PSTATE_SM);
static_assert(offsetof(CpuState, pstate_za) == UML_GADGET_CPU_PSTATE_ZA);
static_assert(offsetof(CpuState, pstate_daif_access) == UML_GADGET_CPU_PSTATE_DAIF_ACCESS);
static_assert(offsetof(CpuState, pstate_daif) == UML_GADGET_CPU_PSTATE_DAIF);
static_assert(offsetof(CpuState, exclusive_address) == UML_GADGET_CPU_EXCLUSIVE_ADDRESS);
static_assert(offsetof(CpuState, exclusive_key) == UML_GADGET_CPU_EXCLUSIVE_KEY);
static_assert(offsetof(CpuState, exclusive_value) == UML_GADGET_CPU_EXCLUSIVE_VALUE);
static_assert(offsetof(CpuState, exclusive_value2) == UML_GADGET_CPU_EXCLUSIVE_VALUE2);
static_assert(offsetof(CpuState, exclusive_version) == UML_GADGET_CPU_EXCLUSIVE_VERSION);
static_assert(offsetof(CpuState, exclusive_size) == UML_GADGET_CPU_EXCLUSIVE_SIZE);
static_assert(offsetof(CpuState, deferred_vectors) == UML_GADGET_CPU_DEFERRED_VECTORS);
static_assert(offsetof(CpuState, deferred_fpcr) == UML_GADGET_CPU_DEFERRED_FPCR);
static_assert(offsetof(CpuState, deferred_fpsr) == UML_GADGET_CPU_DEFERRED_FPSR);
static_assert(offsetof(CpuState, fp_loaded) == UML_GADGET_CPU_FP_LOADED);
static_assert(offsetof(CpuState, fp_dirty) == UML_GADGET_CPU_FP_DIRTY);
static_assert(offsetof(CpuState, fpmr) == UML_GADGET_CPU_FPMR);
static_assert(offsetof(CpuState, gadget_zero) == UML_GADGET_CPU_ZERO);
static_assert(offsetof(CpuState, gadget_host_fpcr) == UML_ARM64_CPU_HOST_FPCR);
static_assert(offsetof(CpuState, gadget_host_fpsr) == UML_ARM64_CPU_HOST_FPSR);
static_assert(offsetof(CpuState, gadget_fp_active) == UML_ARM64_CPU_FP_ACTIVE);
static_assert(offsetof(CpuState, gadget_indirect_miss) == UML_GADGET_CPU_INDIRECT_MISS);
static_assert(offsetof(CpuState, gadget_host_fpmr) == UML_ARM64_CPU_HOST_FPMR);
static_assert(offsetof(CpuState, gadget_fpmr_active) == UML_ARM64_CPU_FPMR_ACTIVE);
static_assert(offsetof(CpuState, gadget_return_stack) == UML_GADGET_CPU_RETURN_STACK);
static_assert(offsetof(CpuState, gadget_return_top) == UML_GADGET_CPU_RETURN_TOP);
static_assert(CpuState::return_stack_entries == UML_GADGET_RETURN_STACK_ENTRIES);
static_assert(offsetof(CpuState, gadget_target_tier) == UML_GADGET_CPU_TARGET_TIER);
static_assert(sizeof(CpuState::ReturnRecord) == 16);

static_assert(sizeof(::uml::memory::TlbEntry) == 32);
static_assert(alignof(::uml::memory::TlbEntry) == 32);
static_assert(offsetof(::uml::memory::Tlb, entries) == UML_GADGET_TLB_ENTRIES);
static_assert(offsetof(::uml::memory::TlbEntry, read_page) == UML_GADGET_TLB_ENTRY_READ_PAGE);
static_assert(offsetof(::uml::memory::TlbEntry, read_data_minus_address) == UML_GADGET_TLB_ENTRY_READ_DATA_MINUS_ADDRESS);
static_assert(offsetof(::uml::memory::TlbEntry, write_page) == UML_GADGET_TLB_ENTRY_WRITE_PAGE);
static_assert(offsetof(::uml::memory::TlbEntry, write_data_minus_address) == UML_GADGET_TLB_ENTRY_WRITE_DATA_MINUS_ADDRESS);
static_assert(offsetof(::uml::memory::Tlb, fault_address) == UML_GADGET_TLB_FAULT_ADDRESS);
static_assert(offsetof(::uml::memory::Tlb, fault_access) == UML_GADGET_TLB_FAULT_ACCESS);

} // namespace uml::cpu::arm64::layout
