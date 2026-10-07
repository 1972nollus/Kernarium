/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_ARM64_GADGETS_SVE_LAYOUT_H
#define UML_ARM64_GADGETS_SVE_LAYOUT_H

/* Constants the gadget engine's SVE part (engine/sve.inc and the gadgets
 * engine/generate_sve.py writes) shares with its C++ side; plain
 * preprocessor definitions, so the assembly can include them.
 *
 * A WHILE* gadget computes how many elements are active, clamps the count
 * to UML_GADGET_SVE_WHILE_COUNTS - 1, and reads the predicate and NZCV it
 * writes from entry `count` of the table its stream names, one table per
 * element size and direction (program_builder_sve.cpp). An entry is
 * 1 << UML_GADGET_SVE_WHILE_ENTRY_SHIFT bytes: the predicate's mask image
 * at UML_GADGET_SVE_WHILE_MASK and NZCV, as the engine keeps it, at
 * UML_GADGET_SVE_WHILE_NZCV. Counts beyond an element size's element count
 * repeat its last entry, so every table takes the one clamp. */
#define UML_GADGET_SVE_WHILE_ENTRY_SHIFT 5
#define UML_GADGET_SVE_WHILE_MASK 0
#define UML_GADGET_SVE_WHILE_NZCV 16
#define UML_GADGET_SVE_WHILE_COUNTS 17

#endif
