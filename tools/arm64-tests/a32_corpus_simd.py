#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The floating-point and Advanced SIMD suites of the AArch32 corpus.

- simd: the register forms of VFPv4-D32 and Advanced SIMD (the `simd` level),
  each decoded word started in recipes whose D registers hold floating-point
  edge values (zeros, infinities, NaNs, denormals, extremes) and patterns,
  under every FPSCR rounding mode, flush-to-zero, default-NaN and half-
  precision setting; the fixed-width integer and element-size variants come
  from sampling the free fields of each form's rows;
- simd-memory: VLDR, VSTR, VLDM, VSTM, the deprecated FLDMX/FSTMX and the
  structure loads and stores VLD1-4/VST1-4, aimed with the memory suites'
  address scenarios, including alignment-hinted accesses at misaligned
  addresses;
- fpscr: VMRS and VMSR, including the exception trap enables the profile
  reads as zero and ignores writes to (decision D4);
- fpscr-reserved: the same with Stride, Len and FZ16 set, whose behaviour is
  the implementation's and so is recorded from hardware only.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from a32_corpus_memory import D, READ_ONLY, solve  # noqa: E402
from a32_corpus_plan import (  # noqa: E402
    FLAGS, ISA_LETTER, Case, Context, candidate_words, fields_of, get_field, pattern, set_field, usable,
    word_of,
)

# FPSCR values a case starts in: rounding modes (23:22), FZ (24), DN (25),
# AHP (26), QC (27) and the cumulative flags. Stride and Len (21:20, 18:16)
# and FZ16 (19) stay clear: whether they stick depends on the implementation,
# so the writes that probe them are the hardware-only fpscr-reserved suite.
FPSCR_MODES = (
    0x00000000, 0x00400000, 0x00800000, 0x00C00000, 0x01000000, 0x02000000, 0x03000000,
    0x04000000, 0x08000000, 0x0000009F, 0x01C00000, 0x07C0009F,
)
FPSCR_RESERVED_BITS = 0x003F0000
# Fields that choose what the instruction does rather than which registers it names.
NOT_VARIANT = {"D", "N", "M", "Vd", "Vn", "Vm", "Rn", "Rm", "Rt", "Rt2", "cond", "P", "W", "imm8", "imm6", "imm4",
               "imm4H", "imm4L", "imm3", "imm3H", "imm3L", "index_align"}
RECIPES = ("edges", "int8", "pattern", "int16", "edges3", "int32", "pattern1", "edges5")
MEMORY_OR_SYSTEM = {"loads", "stores", "exclusive", "barrier", "hint", "prefetch"}


def variant_fields(form):
    return [(field.low, field.width) for field in form.encoding.fields
            if field.name not in NOT_VARIANT and field.width <= 4]


def pick_words(context: Context, form, count: int):
    """Up to `count` usable words of the form, as (word, operands): the first
    candidate, then that word with each variant field (size, U, op, opc, ...)
    taking its other values in turn, then the remaining candidates."""
    candidates = []
    for word in candidate_words(context, form, count + 6):
        found = usable(context, form, word)
        if found is not None and found[0] not in [w for w, _ in candidates]:
            candidates.append(found)
    if not candidates:
        return []
    chosen = [candidates[0]]
    base = candidates[0][0]
    pools = []
    for low, width in variant_fields(form):
        current = get_field(base, low, width)
        pool = []
        for value in range(1 << width):
            if value != current:
                found = usable(context, form, set_field(base, low, width, value))
                if found is not None:
                    pool.append(found)
        if pool:
            pools.append(pool)
    for depth in range(16):
        for pool in pools:
            if depth < len(pool) and pool[depth][0] not in [w for w, _ in chosen] and len(chosen) < count:
                chosen.append(pool[depth])
    for found in candidates[1:]:
        if len(chosen) < count and found[0] not in [w for w, _ in chosen]:
            chosen.append(found)
    return chosen


def simd_forms(context: Context, levels=("simd",)):
    for form in context.model.forms:
        if (form.encoding is None or form.level not in levels or form.availability != "implemented"
                or form.check or form.nop):
            continue
        yield form


def words_for(form) -> int:
    return 10 if variant_fields(form) else 6


def suite_simd(context: Context, levels=("simd",), suite="simd", recipes=RECIPES) -> list[Case]:
    cases = []
    for form in simd_forms(context, levels):
        if form.traits & MEMORY_OR_SYSTEM:
            continue
        isa = form.encoding.isa
        suffix = "a" if isa == "A32" else "t"
        for index, (word, _operands) in enumerate(pick_words(context, form, words_for(form))):
            for variant in range(2):
                number = len(cases)
                recipe = f"{recipes[(index + 2 * variant + number) % len(recipes)]}-{suffix}"
                initial = {"fpscr": FPSCR_MODES[(index * 3 + variant * 5 + number) % len(FPSCR_MODES)],
                           "apsr": FLAGS[pattern(number) >> 8 & 3]}
                cases.append(Case(suite, f"{form.name}/{index}.{variant}", form.name, ISA_LETTER[isa],
                                  word_of(isa, word), 0, recipe, initial))
    return cases


# Memory ----------------------------------------------------------------------------------------------------------------

SCENARIOS = ("a", "x4", "u1", "x16", "ro", "unmapped", "zero", "last")


def extent_of(operands: dict) -> int:
    """The bytes a transfer touches, at most."""
    if "regs" in operands and "imm32" in operands:  # VLDM, VSTM: imm32 is the whole block
        return max(operands["imm32"], 8)
    return 32


def scenario_address(scenario: str, extent: int) -> int:
    half = extent // 2 & ~7  # aligned, so the access straddles the line
    return {
        "a": D + 0x1040, "x4": D + 0x2000 - half, "u1": D + 0x1041, "x16": D + 0x4000 - half,
        "ro": READ_ONLY, "unmapped": 0x60000000, "zero": 0, "last": D + 0x8000 - (extent + 7 & ~7),
    }[scenario]


def aim_memory(operands: dict, address: int, apsr: int, choice: int):
    """Registers that make a SIMD transfer start at `address`."""
    n = operands.get("n")
    if n is None or n == 15:
        return {}
    if "regs" in operands and "imm32" in operands:  # VLDM, VSTM, FLDMX, FSTMX
        return {f"r{n}": (address if operands["add"] else address + operands["imm32"]) & 0xFFFFFFFF}
    if "m" in operands:  # VLDn, VSTn: the address is the base register
        base = {f"r{n}": address & 0xFFFFFFFF}
        m = operands["m"]
        if operands.get("register_index") and m not in (13, 15) and m != n:
            base[f"r{m}"] = (0x20, 0x100, 8, 0x1000)[choice % 4]
        return base
    return solve(operands, address, apsr, choice)  # VLDR, VSTR


def suite_simd_memory(context: Context, levels=("simd",), suite="simd-memory") -> list[Case]:
    cases = []
    used = set()
    for form_index, form in enumerate(simd_forms(context, levels)):
        if not form.traits & {"loads", "stores"} or form.name[0] not in "VF" or form.traits & {"exclusive"}:
            continue
        isa = form.encoding.isa
        suffix = "a" if isa == "A32" else "t"
        for index, (word, operands) in enumerate(pick_words(context, form, 6)):
            for step in range(2):
                number = len(cases)
                scenario = "a" if step == 0 and index % 2 == 0 else \
                    SCENARIOS[(index * 3 + step * 5 + form_index) % len(SCENARIOS)]
                name = f"{form.name}/{index}.{scenario}"
                if name in used:
                    continue
                recipe = f"{RECIPES[(index + step + form_index) % len(RECIPES)]}-{suffix}"
                apsr = FLAGS[pattern(number) >> 8 & 3]
                initial = {"apsr": apsr}
                if operands.get("n") not in (None, 15):
                    aim = aim_memory(operands, scenario_address(scenario, extent_of(operands)), apsr,
                                     number)
                    if aim is None:
                        continue
                    initial.update(aim)
                used.add(name)
                cases.append(Case(suite, name, form.name, ISA_LETTER[isa], word_of(isa, word), 0, recipe,
                                  initial))
    return cases


# VMRS and VMSR ----------------------------------------------------------------------------------------------------------

def fpscr_cases(context: Context, suite: str, values) -> list[Case]:
    cases = []
    for name in ("VMRS_A1_AS", "VMRS_T1_AS", "VMSR_A1_AS", "VMSR_T1_AS"):
        form = context.model.by_name[name]
        isa = form.encoding.isa
        suffix = "a" if isa == "A32" else "t"
        fields = fields_of(form)
        found = None
        for base in candidate_words(context, form, 12):
            found = usable(context, form, base)
            if found is not None:
                break
        for register in (0, 3, 12, 14, 15):
            word = set_field(found[0], *fields["Rt"], register)
            if usable(context, form, word) is None:
                continue
            for index, value in enumerate(values):
                number = len(cases)
                initial = {"apsr": FLAGS[pattern(number) >> 8 & 3], "fpscr": values[(index + 3) % len(values)]}
                if name.startswith("VMSR"):
                    initial[f"r{register}"] = value
                cases.append(Case(suite, f"{name}/r{register}.{index}", name, ISA_LETTER[isa],
                                  word_of(isa, word), 0, f"{RECIPES[index % 4]}-{suffix}", initial))
    return cases


def suite_fpscr(context: Context) -> list[Case]:
    """VMRS and VMSR: the trap-enable bits (14:8 and 15) read as zero and ignore writes."""
    values = (0x00000000, 0x03C09F9F, 0xF7C0FFFF, 0x0000FF00, 0x00001F00,
              0xFFFFFFFF & ~FPSCR_RESERVED_BITS, 0x07C0009F, 0x00C00000, 0x0000009F, 0x08000000)
    return fpscr_cases(context, "fpscr", values)


def suite_fpscr_reserved(context: Context) -> list[Case]:
    """VMRS and VMSR with Stride, Len and FZ16 in play: what sticks is the implementation's."""
    values = (0x00370000, 0xFFFFFFFF, 0x00300000, 0x00080000, 0x003F0000, 0x00010000, 0x00200000, 0x00070000)
    return fpscr_cases(context, "fpscr-reserved", values)


SUITES = {
    "simd": suite_simd,
    "simd-memory": suite_simd_memory,
    "fpscr": suite_fpscr,
    "fpscr-reserved": suite_fpscr_reserved,
}
