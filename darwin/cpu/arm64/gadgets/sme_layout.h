/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_ARM64_GADGETS_SME_LAYOUT_H
#define UML_ARM64_GADGETS_SME_LAYOUT_H

/* Constants the gadget engine's SME part (engine/sme.inc) shares with its
 * C++ side; plain preprocessor definitions, so the assembly can include them.
 *
 * CpuState::sme_host_mode is engine-private. Besides the shared bits
 * (cpu/arm64/sme_profile.hpp), the gadget engine sets this one before it
 * enters a streaming program when the guest has ZT0, so the streaming entry
 * and the gadgets that leave and re-enter streaming mode around a helper
 * know to carry ZT0 with ZA. */
#define UML_GADGET_SME_HOST_ZT0 16

/* The operand of a ZA or ZT0 memory or zeroing gadget, which its slow path
 * hands unchanged to uml_arm64_gadget_sme_memory():
 * - bits 0-15: the CpuState offset of the base register (SP at 248);
 * - bits 16-31: the CpuState offset of the slice index register W12-W15;
 * - bits 32-35: the immediate offset of an array-vector access;
 * - bit 36: the base register is SP;
 * - bits 40-47: the tile mask of ZERO { mask };
 * - bits 48-50: the operation. */
#define UML_GADGET_SME_BASE_SP_BIT 36
#define UML_GADGET_SME_MASK_SHIFT 40
#define UML_GADGET_SME_OPERATION_SHIFT 48
#define UML_GADGET_SME_ZA_LOAD 0
#define UML_GADGET_SME_ZA_STORE 1
#define UML_GADGET_SME_ZT0_LOAD 2
#define UML_GADGET_SME_ZT0_STORE 3
#define UML_GADGET_SME_ZA_ZERO 4
#define UML_GADGET_SME_ZT0_ZERO 5

/* The operand of an immediate SVCR write: the SVCR bits written in bits
 * 0-7 and their new values in bits 8-15. */
#define UML_GADGET_SME_SVCR_VALUE_SHIFT 8

/* What uml_arm64_gadget_sme_write_svcr() returns: the write took effect,
 * the task needs SME storage first (the sme_access exit), or the vCPU has
 * no register file. */
#define UML_GADGET_SME_SVCR_NEEDS_STORAGE 0
#define UML_GADGET_SME_SVCR_WRITTEN 1
#define UML_GADGET_SME_SVCR_FATAL 2

#endif
