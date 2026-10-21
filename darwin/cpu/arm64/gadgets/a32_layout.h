/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_ARM64_GADGETS_A32_LAYOUT_H
#define UML_ARM64_GADGETS_A32_LAYOUT_H

/* Constants the gadget engine's AArch32 part (engine/a32.inc and the gadgets
 * engine/generate_a32.py writes) shares with its C++ side; plain
 * preprocessor definitions, so the assembly can include them.
 *
 * The a32_library gadget runs one instruction through the shared AArch32
 * semantic library (cpu/arm64/aarch32/semantics.hpp). Its stream is
 * [the instruction's key][operand]; a gadget with a slow path that the
 * library takes over carries the operand as its last stream word. The
 * operand holds what the library's decode needs again at run time: the
 * instruction's word in bits 0-31, the ITSTATE it executes in at bit 32,
 * whether the target PC of a load that writes PC is to be left as a value in
 * CpuState::registers[15] at bit 42, whether the word is T32 at bit 43, and
 * the form decoded at build time at bit 48, which the run-time decode must
 * find again. */
#define UML_GADGET_A32_LIBRARY_IT_SHIFT 32
#define UML_GADGET_A32_LIBRARY_PC_VALUE_SHIFT 42
#define UML_GADGET_A32_LIBRARY_THUMB_SHIFT 43
#define UML_GADGET_A32_LIBRARY_FORM_SHIFT 48

/* The single and doubleword transfers, VLDR and VSTR, TBB and TBH, and the
 * block transfers carry no key: bits 24-31 of their operand word hold the
 * instruction's position in its block, which a fault reports as the PC,
 * shifted left by two with bit 0 set, for the executor to resolve to the
 * instruction's key. A block transfer's library path runs with that
 * position in place of the key, because no block transfer the gadgets run
 * reads the PC. */
#define UML_GADGET_A32_POSITION_SHIFT 24

/* What uml_arm64_gadget_a32_execute (a32/library_helpers.cpp) returns to the
 * gadget: the instruction retired; it faulted, with the fault in the TLB;
 * it is UNDEFINED; or it reached an internal error, the engine's fatal
 * event. */
#define UML_GADGET_A32_LIBRARY_RETIRED 0
#define UML_GADGET_A32_LIBRARY_FAULT 1
#define UML_GADGET_A32_LIBRARY_UNDEFINED 2
#define UML_GADGET_A32_LIBRARY_FATAL 3

#endif
