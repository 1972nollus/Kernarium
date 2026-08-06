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
