#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Armv8 additions of the AArch32 corpus (the `complete` level).

- armv8: the register forms (CRC32, AES, SHA-1, SHA-2, VRINT*, VCVT[ANPM],
  VSEL, VMAXNM, VMINNM, the half-precision arithmetic, VCADD, VCMLA, VQRDMLAH,
  VQRDMLSH, VJCVT, the dot-product, matrix-multiply and half-to-single
  multiply-accumulate forms), started in the simd suite's floating-point
  states under every FPSCR mode;
- armv8-memory: LDA and STL (acquire and release loads and stores) and the
  half-precision VLDR and VSTR, with the memory suites' address scenarios;
- armv8-exclusive: LDAEX and STLEX sequences like the exclusive suite's;
- armv8-system: SB and SEVL.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import a32_corpus_memory as memory  # noqa: E402
import a32_corpus_simd as simd  # noqa: E402
import a32_corpus_system as system  # noqa: E402
from a32_corpus_plan import Case, Context  # noqa: E402

LEVELS = ("complete",)
RECIPES = ("edges", "half", "int8", "pattern", "half", "int16", "edges3", "int32", "pattern1", "edges5")


def suite_armv8(context: Context) -> list[Case]:
    return simd.suite_simd(context, LEVELS, "armv8", RECIPES)


def suite_armv8_memory(context: Context) -> list[Case]:
    cases = memory.build(context, "complete", "armv8-memory", True)
    cases += simd.suite_simd_memory(context, LEVELS, "armv8-memory")
    return cases


def suite_armv8_exclusive(context: Context) -> list[Case]:
    return memory.suite_exclusive(context, "armv8-exclusive", "LDAEX", "STLEX")


def suite_armv8_system(context: Context) -> list[Case]:
    forms = [form for form in context.model.forms
             if form.encoding is not None and form.level == "complete" and form.availability == "implemented"
             and (form.traits & {"barrier", "hint"} or form.nop) and not form.check]
    return system.generic(context, "armv8-system", forms, 6, 2)


SUITES = {
    "armv8": suite_armv8,
    "armv8-memory": suite_armv8_memory,
    "armv8-exclusive": suite_armv8_exclusive,
    "armv8-system": suite_armv8_system,
}
