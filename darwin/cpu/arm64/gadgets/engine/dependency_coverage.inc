/* SPDX-License-Identifier: GPL-2.0-or-later */
#if UML_GADGET_EXECUTION_HISTOGRAM
/* Diagnostic only. Forwarded x0/v2 results and architectural flags are
 * untouched. The entry frame owns a carrier-local counter pointer. No
 * helper or non-gadget exit is needed per observed decode group. */
gadget dependency_coverage
	ldp x8, x9, [_stream], #16
	ldr x10, [sp, #0x70]
	ldr x11, [x10]
	add x11, x11, x9
	str x11, [x10]
	ldr x11, [x10, x8]
	add x11, x11, #1
	str x11, [x10, x8]
	next_gadget
#endif
