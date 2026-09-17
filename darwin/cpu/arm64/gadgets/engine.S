/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Keep one translation unit so shared macros and local labels remain cheap. */
#include "engine/core_integer.inc"
/* Keep the FP/SIMD bank on its established 64-byte cache-line phase after
 * factoring the native-environment cold path into one shared routine. */
.space 16
#include "engine/floating_simd.inc"
#include "engine/system_atomic.inc"
#include "engine/load_store.inc"
#include "engine/control.inc"
/* Experimental entries are appended so adding a candidate cannot move or
 * rephase any established gadget. */
#include "engine/experimental.inc"
#include "engine/inplace_immediate.inc"
#include "engine/inplace_extended.inc"
#include "engine/template_copies.inc"
#include "engine/inplace_memory.inc"
#include "engine/scalar_forwarding.inc"
#include "engine/vector_forwarding.inc"
#include "engine/dependency_coverage.inc"
