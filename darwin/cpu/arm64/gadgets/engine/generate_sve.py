#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the gadget engine's non-streaming SVE gadgets.

No supported host implements SVE, so a program the gadget engine translates
for SVCR.SM = 0 runs every SVE form it implements on AdvSIMD and general
registers, at the virtual CPU's fixed vector length of 16 bytes
(cpu/arm64/sve_profile.hpp). Z_n is CpuState::vectors[n]; P0-P15 and FFR are
CpuState's 16-byte mask images, 0xff in each active byte. This generator
writes, at build time from the shared encoding table
(darwin/cpu/arm64/detail/sme_encodings.hpp), one gadget per form the engine
implements and per value of the bits that select its lowering (element
size, merging or zeroing, a fused branch's condition), and a header that
maps each table row to its form.

A gadget's stream holds the CpuState offsets of the registers its form
names, 16 bits each in slot order, and then any immediates the builder
computes: a predicate image, a count from the vector length, a replicated
immediate, a TBL index vector, or the address of a WHILE* table
(sve_layout.h). Some forms need no gadget of their own: CNT*, RDVL and the
scalar INC* and DEC* forms are moves and additions of a builder constant,
and ADDVL and ADDPL additions to a register or SP, so they select the base
part's entries.

Predicate lane masks for elements wider than a byte come from one CMTST
against the element pattern, and the flag-setting forms compute PredTest
in engine/sve.inc's sve_predicate_test, so the guest's NZCV stays in x27
as everywhere in the engine. Nothing here is an SVE instruction: the SVE
part assembles with SVE and SME off.

A variant the generator does not lower, such as an element size AdvSIMD
has no form for, is absent: its entry is
uml_arm64_gadget_sve_generated_absent, which nothing executes, and the
builder runs the word through the semantic library instead. The rules
each family follows (all-true tests, benign inactive inputs, saturation
without FPSR.QC) are in its comments and docs/architecture.md.

usage: generate_sve.py --loader LOADER --table TABLE
                       --assembly OUT.inc --header OUT.hpp
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import itertools
import sys
from dataclasses import dataclass, field as dataclass_field
from pathlib import Path

# Element sizes in bytes, by the size field's value.
ESIZES = (1, 2, 4, 8)
ARRANGEMENTS = {1: "16b", 2: "8h", 4: "4s", 8: "2d"}
# The lower half of a vector of elements of a size, as a widening operation
# reads it.
HALVES = {1: "8b", 2: "4h", 4: "2s"}
# A variant the generator leaves to the semantic library: its entry is the
# absent marker, and the builder emits the library gadget for the word.
ABSENT = None
SIZE_BITS = [22, 23]
SF_BIT = 12

# How the builder fills a form's stream after its slot offsets
# (program_builder_sve.cpp). Base lowerings select base entries.
LOWERINGS = ["plain", "predicate_constant", "predicate_constant_flags",
             "while_compare", "while_conflict", "vector_count",
             "integer_immediate", "bitmask_immediate", "float_immediate",
             "index_table", "count_move", "count_add",
             "vector_length_add", "unsigned_immediate", "memory",
             "prefetch"]

# Form flags.
FLAG_DISCARD_ON_ZERO = 1  # slot 0 is a general destination; XZR is a NOP.

# The conditions a WHILE* fuses with: B.cond tests one NZCV bit of x27.
FUSED_CONDITIONS = (("eq", 30, True), ("ne", 30, False),
                    ("mi", 31, True), ("pl", 31, False))


class GenerationError(Exception):
    pass


@dataclass(frozen=True)
class Segment:
    low: int
    width: int
    constant: int
    field: bool


@dataclass(frozen=True)
class Slot:
    kind: str
    segments: tuple


@dataclass
class Form:
    name: str
    rows: list
    lowering: str
    slots: list
    key: str = ""
    variant_bits: list = dataclass_field(default_factory=list)
    variants: list = dataclass_field(default_factory=list)
    argument: int = 0
    flags: int = 0
    first_entry: int = 0


def load_table(loader: Path, table: Path):
    spec = importlib.util.spec_from_file_location("sme_encoding_table", loader)
    module = importlib.util.module_from_spec(spec)
    sys.modules["sme_encoding_table"] = module
    spec.loader.exec_module(module)
    return module.load(table)


def slots_of(row, *wanted: tuple) -> list:
    """The row's operands in the order a template reads them, each named by
    its kind and the low bit of its first field."""
    result = []
    for kind, low in wanted:
        for operand in row.operands:
            fields = [s for s in operand.segments if s.field]
            if operand.kind == kind and fields and fields[0].low == low:
                result.append(Slot(kind, tuple(
                    Segment(s.low, s.width, s.constant, s.field)
                    for s in operand.segments)))
                break
        else:
            raise GenerationError(f"{row.encoding}: no {kind} operand at "
                                  f"bit {low}")
    if len(result) != len(row.operands):
        raise GenerationError(f"{row.encoding}: {len(row.operands)} "
                              f"operands, template reads {len(result)}")
    return result


# Templates. Slot i's offset is in x3 + i after unpack(); x8 holds the
# packed word. x9-x17 and v0-v7 are scratch; x27 is the guest's NZCV.

def unpack(count: int, skip: tuple = ()) -> list:
    lines = []
    for slot in range(count):
        if slot in skip:
            continue
        target = f"x{3 + slot}"
        if slot == 0:
            lines.append(f"and {target}, x8, #0xffff")
        elif slot == count - 1:
            lines.append(f"lsr {target}, x8, #{16 * slot}")
        else:
            lines.append(f"ubfx {target}, x8, #{16 * slot}, #16")
    return lines


def gpr(esize: int, number: int) -> str:
    return f"{'x' if esize == 8 else 'w'}{number}"


def active_count(esize: int, source: str = "v0") -> list:
    """w9 := the active elements of `esize` bytes in predicate `source`."""
    lines = []
    if esize > 1:
        lines += ["sve_element_pattern %d, 7" % esize,
                  f"and {source}.16b, {source}.16b, v7.16b"]
    return lines + [f"cnt {source}.16b, {source}.16b",
                    f"addv b{source[1:]}, {source}.16b",
                    f"fmov w9, s{source[1:]}", "lsr w9, w9, #3"]


PREDICATE_LOGICAL = {
    # name: (lines on v1 = Pn, v2 = Pm, v0 = Pg, result register)
    "and": (["and v1.16b, v1.16b, v2.16b", "and v1.16b, v1.16b, v0.16b"],
            "v1"),
    "bic": (["bic v1.16b, v1.16b, v2.16b", "and v1.16b, v1.16b, v0.16b"],
            "v1"),
    "eor": (["eor v1.16b, v1.16b, v2.16b", "and v1.16b, v1.16b, v0.16b"],
            "v1"),
    "orr": (["orr v1.16b, v1.16b, v2.16b", "and v1.16b, v1.16b, v0.16b"],
            "v1"),
    "orn": (["orn v1.16b, v1.16b, v2.16b", "and v1.16b, v1.16b, v0.16b"],
            "v1"),
    "nand": (["and v1.16b, v1.16b, v2.16b", "bic v1.16b, v0.16b, v1.16b"],
             "v1"),
    "nor": (["orr v1.16b, v1.16b, v2.16b", "bic v1.16b, v0.16b, v1.16b"],
            "v1"),
    "sel": (["bsl v0.16b, v1.16b, v2.16b"], "v0"),
}


def predicate_test(mask: str, result: str) -> list:
    """NZCV := PredTest(mask, result, 8) for two predicates in V
    registers."""
    return [f"fmov x9, d{mask[1:]}", f"mov x10, {mask}.d[1]",
            f"fmov x11, d{result[1:]}", f"mov x12, {result}.d[1]",
            "sve_predicate_test"]


def predicate_logical(name: str, flags: bool) -> list:
    lines, result = PREDICATE_LOGICAL[name]
    body = ["ldr x8, [_stream], #8"] + unpack(4, skip=(0,)) + [
        "ldr q1, [_cpu, x4]", "ldr q2, [_cpu, x5]", "ldr q0, [_cpu, x6]"]
    body += lines + ["and x3, x8, #0xffff",
                     f"str q{result[1:]}, [_cpu, x3]"]
    if flags:
        body += predicate_test("v0", result)
    return body


def while_compare(name: str, sf: int, fused: tuple | None) -> list:
    """WHILE* of two general registers: the active count, clamped to 16,
    indexes the table the stream names (sve_layout.h)."""
    operation = name[5:7]
    signed = operation in ("lt", "le", "gt", "ge")
    inclusive = operation in ("le", "ls", "ge", "hs")
    decrement = operation in ("gt", "ge", "hi", "hs")
    body = ["ldp x8, x12, [_stream], #16"] + \
        ["ubfx x4, x8, #16, #16", "ubfx x5, x8, #32, #16"]
    for target, slot in (("9", "x4"), ("10", "x5")):
        if sf:
            body.append(f"ldr x{target}, [_cpu, {slot}]")
        elif signed:
            body.append(f"ldrsw x{target}, [_cpu, {slot}]")
        else:
            body.append(f"ldr w{target}, [_cpu, {slot}]")
    # x9 = operand1, which steps towards operand2 (x10) one element at a
    # time; the elements active are those before the comparison first
    # fails, which it does before operand1 wraps unless an inclusive
    # comparison's bound is the extreme value, when every element is.
    first, second = ("x10", "x9") if not decrement else ("x9", "x10")
    holds = {("lt", False): "lt", ("le", False): "le", ("lo", False): "lo",
             ("ls", False): "ls", ("gt", True): "gt", ("ge", True): "ge",
             ("hi", True): "hi", ("hs", True): "hs"}[(operation, decrement)]
    body += ["mov x13, #16", f"sub x11, {first}, {second}"]
    if inclusive:
        # distance + 1 elements, at most 16, when the comparison holds.
        body += ["cmp x11, #15", "csinc x11, x13, x11, hs"]
    else:
        body += ["cmp x11, #16", "csel x11, x11, x13, lo"]
    body += ["cmp x9, x10", f"csel x11, x11, xzr, {holds}"]
    if inclusive:
        extreme = {("le", 1): "#0x7fffffffffffffff",
                   ("le", 0): "#0x7fffffff",
                   ("ge", 1): "#0x8000000000000000",
                   ("ge", 0): "#0xffffffff80000000"}
        if operation in ("le", "ge"):
            body += [f"mov x14, {extreme[(operation, sf)]}", "cmp x10, x14"]
        elif operation == "ls":
            body.append("cmn x10, #1" if sf else "cmn w10, #1")
        else:  # hs: the bound 0
            body.append("cmp x10, #0")
        body.append("csel x11, x13, x11, eq")
    body += while_store(fused)
    return body


def while_store(fused: tuple | None) -> list:
    body = ["add x12, x12, x11, lsl #UML_GADGET_SVE_WHILE_ENTRY_SHIFT",
            "ldr q0, [x12, #UML_GADGET_SVE_WHILE_MASK]",
            "ldr w27, [x12, #UML_GADGET_SVE_WHILE_NZCV]",
            "and x3, x8, #0xffff", "str q0, [_cpu, x3]"]
    if fused is None:
        return body + ["next_gadget"]
    _, bit, taken_when_set = fused
    # The taken link follows; the instructions translated after the pair
    # are in the operand's top 16 bits (conditional_branch_inline).
    return body + ["ldr x19, [_stream], #8", "lsr x20, x8, #48",
                   f"{'tbnz' if taken_when_set else 'tbz'} w27, #{bit}, 99f",
                   "next_gadget", "99:", "sub x29, x29, x20",
                   "sve_follow_block_link x19"]


def while_conflict(name: str, esize: int) -> list:
    """WHILEWR and WHILERW: every element active when the distance in
    elements is 0 (or for WHILEWR negative), else that many."""
    shift = esize.bit_length() - 1
    body = ["ldp x8, x12, [_stream], #16", "ubfx x4, x8, #16, #16",
            "ubfx x5, x8, #32, #16", "ldr x9, [_cpu, x4]",
            "ldr x10, [_cpu, x5]", "mov x13, #16",
            "subs x11, x10, x9"]
    if name.startswith("whilerw"):
        body += ["sub x14, x9, x10", "csel x11, x11, x14, hs"]
    else:
        # A borrow is a negative distance.
        body.append("csel x11, x11, xzr, hs")
    if shift:
        body.append(f"lsr x11, x11, #{shift}")
    body += ["cmp x11, #16", "csel x11, x11, x13, lo",
             "cmp x11, #0", "csel x11, x13, x11, eq"]
    return body + while_store(None)


def lane_mask(esize: int, register: int = 0) -> list:
    return [] if esize == 1 else [f"sve_lane_mask {esize}, {register}, 7"]


def three_register(lines: list, accumulate: bool = False,
                   fp: bool = False) -> list:
    """Zd := f(Zn, Zm): slot 0 Zd in x3, slot 1 Zn in x4 (v1), slot 2 Zm in
    x5 (v2); `lines` leave the result in v0, which holds Zd first when
    `accumulate`. A floating-point form runs in the guest's environment."""
    body = ["ensure_fp_environment"] if fp else []
    body += ["ldr x8, [_stream], #8"] + unpack(3, skip=(0,)) + [
        "ldr q1, [_cpu, x4]", "ldr q2, [_cpu, x5]", "and x3, x8, #0xffff"]
    if accumulate:
        body.append("ldr q0, [_cpu, x3]")
    return body + lines + ["str q0, [_cpu, x3]", "next_gadget"]


def same_size(operation: str):
    """The AdvSIMD instruction of the same name on whole vectors."""
    return lambda e: [f"{operation} v0.{ARRANGEMENTS[e]}, "
                      f"v1.{ARRANGEMENTS[e]}, v2.{ARRANGEMENTS[e]}"]


def unsigned_saturating_add(e: int) -> list:
    """UQADD without AdvSIMD's: the sum wraps below an operand exactly
    when it saturates. SVE sets no FPSR.QC, which UQADD would."""
    a = ARRANGEMENTS[e]
    return [f"add v0.{a}, v1.{a}, v2.{a}", f"cmhi v3.{a}, v1.{a}, v0.{a}",
            "orr v0.16b, v0.16b, v3.16b"]


def unsigned_saturating_subtract(e: int) -> list:
    """UQSUB as max(a, b) - b, or with a borrow mask for doublewords."""
    a = ARRANGEMENTS[e]
    if e == 8:
        return [f"cmhi v3.{a}, v1.{a}, v2.{a}", f"sub v0.{a}, v1.{a}, v2.{a}",
                "and v0.16b, v0.16b, v3.16b"]
    return [f"umax v0.{a}, v1.{a}, v2.{a}", f"sub v0.{a}, v0.{a}, v2.{a}"]


def doubleword_scalar(operation: str) -> list:
    """v0 := `operation` of v1 and v2 lane by lane, for a doubleword form
    AdvSIMD lacks."""
    return ["fmov x9, d1", "fmov x10, d2", f"{operation} x9, x9, x10",
            "mov x11, v1.d[1]", "mov x12, v2.d[1]",
            f"{operation} x11, x11, x12", "fmov d0, x9",
            "mov v0.d[1], x11"]


def multiply(e: int) -> list:
    if e == 8:
        return doubleword_scalar("mul")
    return same_size("mul")(e)


def multiply_high(signed: bool):
    """The high half of each product: the widening products of both halves,
    then their odd elements."""
    prefix = "s" if signed else "u"

    def lines(e: int) -> list:
        if e == 8:
            return doubleword_scalar(f"{prefix}mulh")
        wide = ARRANGEMENTS[2 * e]
        return [f"{prefix}mull v3.{wide}, v1.{HALVES[e]}, v2.{HALVES[e]}",
                f"{prefix}mull2 v4.{wide}, v1.{ARRANGEMENTS[e]}, "
                f"v2.{ARRANGEMENTS[e]}",
                f"uzp2 v0.{ARRANGEMENTS[e]}, v3.{ARRANGEMENTS[e]}, "
                f"v4.{ARRANGEMENTS[e]}"]
    return lines


def widening(operation: str, top: bool):
    """An SVE2 bottom (even) or top (odd) widening form: its narrow
    elements to the lower half, then the AdvSIMD long form. The size field
    names the wide element."""
    def lines(e: int):
        if e == 1:
            return ABSENT
        narrow = e // 2
        pick = "uzp2" if top else "uzp1"
        n = ARRANGEMENTS[narrow]
        return [f"{pick} v1.{n}, v1.{n}, v1.{n}",
                f"{pick} v2.{n}, v2.{n}, v2.{n}",
                f"{operation} v0.{ARRANGEMENTS[e]}, v1.{HALVES[narrow]}, "
                f"v2.{HALVES[narrow]}"]
    return lines


def byte_mask(e: int, high: bool) -> int:
    """The doubleword whose bytes are 0xff in the high (or low) half of each
    element of `e` bytes."""
    half = e // 2
    value = 0
    for byte in range(8):
        if (byte % e >= half) == high:
            value |= 0xff << (8 * byte)
    return value


def add_high_narrow(top: bool):
    """ADDHNB: the sum's high half moved down, the top half zero; ADDHNT:
    the sum's high half kept in place under the bottom half of Zd."""
    def lines(e: int):
        if e == 1:
            return ABSENT
        a = ARRANGEMENTS[e]
        if not top:
            return [f"add v0.{a}, v1.{a}, v2.{a}",
                    f"ushr v0.{a}, v0.{a}, #{4 * e}"]
        return [f"add v3.{a}, v1.{a}, v2.{a}",
                f"movi v4.2d, #0x{byte_mask(e, True):016x}",
                "bit v0.16b, v3.16b, v4.16b"]
    return lines


# The register-only forms of three Z registers, each Zd, Zn, Zm, sized by
# bits 22-23: name: (lines by element size, whether Zd is read, whether it
# is floating-point).
THREE_REGISTER = {
    "add_z_zz_": (same_size("add"), False, False),
    "sub_z_zz_": (same_size("sub"), False, False),
    "uqadd_z_zz_": (unsigned_saturating_add, False, False),
    "uqsub_z_zz_": (unsigned_saturating_subtract, False, False),
    "zip1_z_zz_": (same_size("zip1"), False, False),
    "zip2_z_zz_": (same_size("zip2"), False, False),
    "uzp1_z_zz_": (same_size("uzp1"), False, False),
    "uzp2_z_zz_": (same_size("uzp2"), False, False),
    "trn1_z_zz_": (same_size("trn1"), False, False),
    "trn2_z_zz_": (same_size("trn2"), False, False),
    "mul_z_zz_": (multiply, False, False),
    "umulh_z_zz_": (multiply_high(False), False, False),
    "smulh_z_zz_": (multiply_high(True), False, False),
    "umullb_z_zz_": (widening("umull", False), False, False),
    "umullt_z_zz_": (widening("umull", True), False, False),
    "smullb_z_zz_": (widening("smull", False), False, False),
    "smullt_z_zz_": (widening("smull", True), False, False),
    "uaddlb_z_zz_": (widening("uaddl", False), False, False),
    "uaddlt_z_zz_": (widening("uaddl", True), False, False),
    "saddlb_z_zz_": (widening("saddl", False), False, False),
    "saddlt_z_zz_": (widening("saddl", True), False, False),
    "usublb_z_zz_": (widening("usubl", False), False, False),
    "usublt_z_zz_": (widening("usubl", True), False, False),
    "ssublb_z_zz_": (widening("ssubl", False), False, False),
    "ssublt_z_zz_": (widening("ssubl", True), False, False),
    "umlalb_z_zzz_": (widening("umlal", False), True, False),
    "umlalt_z_zzz_": (widening("umlal", True), True, False),
    "smlalb_z_zzz_": (widening("smlal", False), True, False),
    "smlalt_z_zzz_": (widening("smlal", True), True, False),
    "umlslb_z_zzz_": (widening("umlsl", False), True, False),
    "umlslt_z_zzz_": (widening("umlsl", True), True, False),
    "smlslb_z_zzz_": (widening("smlsl", False), True, False),
    "smlslt_z_zzz_": (widening("smlsl", True), True, False),
    "addhnb_z_zz_": (add_high_narrow(False), False, False),
    "addhnt_z_zz_": (add_high_narrow(True), True, False),
    "tbl_z_zz_1": (lambda e: ["tbl v0.16b, {v1.16b}, v2.16b"] if e == 1
                   else ABSENT, False, False),
}


# Floating-point forms. A predicated one tests its governing predicate for
# all-true first: every element active runs the unpredicated AdvSIMD form;
# otherwise the inactive elements of every input become benign values, +0.0
# or a divisor's 1.0, so that they raise nothing, and the result merges
# under the lane mask.

ELEMENT_PATTERNS = {2: 0x00ff00ff00ff00ff, 4: 0x000000ff000000ff, 8: 0xff}


def all_true_test(e: int, predicate: str) -> list:
    """Branch to 1f unless every element of `e` bytes of the predicate at
    CpuState offset `predicate` is active."""
    lines = [f"add x9, _cpu, {predicate}", "ldp x10, x11, [x9]",
             "and x10, x10, x11"]
    if e == 1:
        return lines + ["cmn x10, #1", "b.ne 1f"]
    pattern = ELEMENT_PATTERNS[e]
    return lines + [f"and x10, x10, #0x{pattern:x}",
                    f"eor x10, x10, #0x{pattern:x}", "cbnz x10, 1f"]


def fp_sized(template):
    """Half, single and double precision, by the size field; size 0 has no
    floating-point form."""
    return lambda e: ABSENT if e == 1 else template(e)


def fp_binary_predicated(operation: str, divisor: str = ""):
    """Zdn := Zdn op Zm under Pg (merging): slots Zdn (x3), Zm (x4), Pg
    (x5). `divisor` names the operand, a or b, that a partial predicate's
    inactive elements set to 1.0."""
    def lines(e: int) -> list:
        a = ARRANGEMENTS[e]
        body = ["ensure_fp_environment", "ldr x8, [_stream], #8"] + \
            unpack(3) + ["ldr q1, [_cpu, x3]", "ldr q2, [_cpu, x4]"] + \
            all_true_test(e, "x5") + [
                f"{operation} v0.{a}, v1.{a}, v2.{a}",
                "str q0, [_cpu, x3]", "next_gadget", "1:",
                "ldr q0, [_cpu, x5]"] + lane_mask(e) + [
                "and v3.16b, v1.16b, v0.16b", "and v4.16b, v2.16b, v0.16b"]
        if divisor:
            target = "v3" if divisor == "a" else "v4"
            body += [f"fmov v5.{a}, #1.0",
                     f"bif {target}.16b, v5.16b, v0.16b"]
        return body + [f"{operation} v5.{a}, v3.{a}, v4.{a}",
                       "bit v1.16b, v5.16b, v0.16b", "str q1, [_cpu, x3]",
                       "next_gadget"]
    return fp_sized(lines)


def fp_ternary_predicated(operation: str, negate: bool, addend: int,
                          multiplicand: int, multiplier: int,
                          destination: int):
    """A fused multiply-add under Pg (merging), its four slots in x3-x6
    with Pg last: v0 := addend (negated first when `negate`) plus or minus
    multiplicand times multiplier, by `operation` (fmla or fmls), merged
    into the destination slot, which also holds the addend or the
    multiplicand."""
    def lines(e: int) -> list:
        a = ARRANGEMENTS[e]
        body = ["ensure_fp_environment", "ldr x8, [_stream], #8"] + \
            unpack(4) + [f"ldr q0, [_cpu, x{3 + addend}]",
                         f"ldr q1, [_cpu, x{3 + multiplicand}]",
                         f"ldr q2, [_cpu, x{3 + multiplier}]"] + \
            all_true_test(e, "x6")
        body += ([f"fneg v0.{a}, v0.{a}"] if negate else []) + [
            f"{operation} v0.{a}, v1.{a}, v2.{a}",
            f"str q0, [_cpu, x{3 + destination}]", "next_gadget", "1:",
            "ldr q3, [_cpu, x6]"] + lane_mask(e, 3) + [
            "and v4.16b, v0.16b, v3.16b", "and v5.16b, v1.16b, v3.16b",
            "and v6.16b, v2.16b, v3.16b"]
        body += ([f"fneg v4.{a}, v4.{a}"] if negate else []) + [
            f"{operation} v4.{a}, v5.{a}, v6.{a}"]
        original = "v0" if destination == addend else "v1"
        return body + [f"bit {original}.16b, v4.16b, v3.16b",
                       f"str q{original[1:]}, [_cpu, x{3 + destination}]",
                       "next_gadget"]
    return fp_sized(lines)


def fp_unary_predicated(operation: str, esize: int = 0):
    """Zd := op(Zn) under Pg (merging): slots Zd (x3), Zn (x4), Pg (x5).
    A conversion between equal sizes has its one size `esize`."""
    def lines(e: int) -> list:
        a = ARRANGEMENTS[e]
        return ["ensure_fp_environment", "ldr x8, [_stream], #8"] + \
            unpack(3) + ["ldr q1, [_cpu, x4]"] + all_true_test(e, "x5") + [
                f"{operation} v0.{a}, v1.{a}", "str q0, [_cpu, x3]",
                "next_gadget", "1:", "ldr q0, [_cpu, x5]"] + lane_mask(e) + [
                "and v3.16b, v1.16b, v0.16b", f"{operation} v4.{a}, v3.{a}",
                "ldr q2, [_cpu, x3]", "bit v2.16b, v4.16b, v0.16b",
                "str q2, [_cpu, x3]", "next_gadget"]
    return (lambda e: lines(esize)) if esize else fp_sized(lines)


# name: lines by element size.
FP_BINARY_PREDICATED = {
    "fadd_z_p_zz_": fp_binary_predicated("fadd"),
    "fsub_z_p_zz_": fp_binary_predicated("fsub"),
    "fmul_z_p_zz_": fp_binary_predicated("fmul"),
    "fmulx_z_p_zz_": fp_binary_predicated("fmulx"),
    "fmax_z_p_zz_": fp_binary_predicated("fmax"),
    "fmin_z_p_zz_": fp_binary_predicated("fmin"),
    "fmaxnm_z_p_zz_": fp_binary_predicated("fmaxnm"),
    "fminnm_z_p_zz_": fp_binary_predicated("fminnm"),
    "fabd_z_p_zz_": fp_binary_predicated("fabd"),
    "fdiv_z_p_zz_": fp_binary_predicated("fdiv", "b"),
}

# name: (operation, negate, addend, multiplicand, multiplier, destination)
# over the slots Zd-or-Zdn (0), the next two Z (1, 2), and Pg (3).
FP_TERNARY_PREDICATED = {
    "fmla_z_p_zzz_": ("fmla", False, 0, 1, 2, 0),
    "fmls_z_p_zzz_": ("fmls", False, 0, 1, 2, 0),
    "fnmla_z_p_zzz_": ("fmls", True, 0, 1, 2, 0),
    "fnmls_z_p_zzz_": ("fmla", True, 0, 1, 2, 0),
    "fmad_z_p_zzz_": ("fmla", False, 2, 0, 1, 0),
    "fmsb_z_p_zzz_": ("fmls", False, 2, 0, 1, 0),
    "fnmad_z_p_zzz_": ("fmls", True, 2, 0, 1, 0),
    "fnmsb_z_p_zzz_": ("fmla", True, 2, 0, 1, 0),
}

FP_UNARY_PREDICATED = {
    "fabs_z_p_z_m": fp_unary_predicated("fabs"),
    "fneg_z_p_z_m": fp_unary_predicated("fneg"),
    "fsqrt_z_p_z_m": fp_unary_predicated("fsqrt"),
    "frintn_z_p_z_m": fp_unary_predicated("frintn"),
    "frintp_z_p_z_m": fp_unary_predicated("frintp"),
    "frintm_z_p_z_m": fp_unary_predicated("frintm"),
    "frintz_z_p_z_m": fp_unary_predicated("frintz"),
    "frinta_z_p_z_m": fp_unary_predicated("frinta"),
    "frintx_z_p_z_m": fp_unary_predicated("frintx"),
    "frinti_z_p_z_m": fp_unary_predicated("frinti"),
    "scvtf_z_p_z_h2fp16": fp_unary_predicated("scvtf", 2),
    "ucvtf_z_p_z_h2fp16": fp_unary_predicated("ucvtf", 2),
    "scvtf_z_p_z_w2s": fp_unary_predicated("scvtf", 4),
    "ucvtf_z_p_z_w2s": fp_unary_predicated("ucvtf", 4),
    "scvtf_z_p_z_x2d": fp_unary_predicated("scvtf", 8),
    "ucvtf_z_p_z_x2d": fp_unary_predicated("ucvtf", 8),
    "fcvtzs_z_p_z_fp162h": fp_unary_predicated("fcvtzs", 2),
    "fcvtzu_z_p_z_fp162h": fp_unary_predicated("fcvtzu", 2),
    "fcvtzs_z_p_z_s2w": fp_unary_predicated("fcvtzs", 4),
    "fcvtzu_z_p_z_s2w": fp_unary_predicated("fcvtzu", 4),
    "fcvtzs_z_p_z_d2x": fp_unary_predicated("fcvtzs", 8),
    "fcvtzu_z_p_z_d2x": fp_unary_predicated("fcvtzu", 8),
}

# The indexed forms: name: (operation, element letter, index bits).
FP_INDEXED = {
    "fmla_z_zzzi_h": ("fmla", "h", [19, 20, 22]),
    "fmla_z_zzzi_s": ("fmla", "s", [19, 20]),
    "fmla_z_zzzi_d": ("fmla", "d", [20]),
    "fmls_z_zzzi_h": ("fmls", "h", [19, 20, 22]),
    "fmls_z_zzzi_s": ("fmls", "s", [19, 20]),
    "fmls_z_zzzi_d": ("fmls", "d", [20]),
    "fmul_z_zzi_h": ("fmul", "h", [19, 20, 22]),
    "fmul_z_zzi_s": ("fmul", "s", [19, 20]),
    "fmul_z_zzi_d": ("fmul", "d", [20]),
}

THREE_REGISTER.update({
    "fadd_z_zz_": (fp_sized(same_size("fadd")), False, True),
    "fsub_z_zz_": (fp_sized(same_size("fsub")), False, True),
    "fmul_z_zz_": (fp_sized(same_size("fmul")), False, True),
    "frecps_z_zz_": (fp_sized(same_size("frecps")), False, True),
    "frsqrts_z_zz_": (fp_sized(same_size("frsqrts")), False, True),
})


# Predicated integer forms merge their result under the lane mask, which
# costs less than the all-true test would. An operation's lines put
# `d` := op(a, b) for element size e, with v4-v6 and x9-x14 scratch.

def vector_op(operation: str):
    return lambda e, d, a, b: [f"{operation} {d}.{ARRANGEMENTS[e]}, "
                               f"{a}.{ARRANGEMENTS[e]}, {b}.{ARRANGEMENTS[e]}"]


def bitwise_op(operation: str):
    return lambda e, d, a, b: [f"{operation} {d}.16b, {a}.16b, {b}.16b"]


def lanes_in_gprs(operation: str):
    """The doubleword lanes through general registers, for an operation
    AdvSIMD lacks at that size."""
    return lambda e, d, a, b: [
        f"fmov x9, d{a[1:]}", f"fmov x10, d{b[1:]}",
        f"{operation} x9, x9, x10", f"mov x11, {a}.d[1]",
        f"mov x12, {b}.d[1]", f"{operation} x11, x11, x12",
        f"fmov d{d[1:]}, x9", f"mov {d}.d[1], x11"]


def doubleword_by(native, doubleword):
    return lambda e, d, a, b: (doubleword if e == 8 else native)(e, d, a, b)


def select_by(compare: str, swap: bool):
    """d := a where compare(a, b) holds (or compare(b, a) when `swap`),
    else b: a doubleword maximum or minimum."""
    def lines(e, d, a, b):
        first, second = (b, a) if swap else (a, b)
        return [f"{compare} v6.2d, {first}.2d, {second}.2d",
                f"bsl v6.16b, {a}.16b, {b}.16b", f"mov {d}.16b, v6.16b"]
    return lines


def absolute_difference(compare: str):
    def lines(e, d, a, b):
        return [f"{compare} v6.2d, {a}.2d, {b}.2d",
                f"sub v5.2d, {a}.2d, {b}.2d", f"sub v4.2d, {b}.2d, {a}.2d",
                "bsl v6.16b, v5.16b, v4.16b", f"mov {d}.16b, v6.16b"]
    return lines


def multiply_high_into(signed: bool):
    prefix = "s" if signed else "u"

    def lines(e, d, a, b):
        if e == 8:
            return lanes_in_gprs(f"{prefix}mulh")(e, d, a, b)
        wide = ARRANGEMENTS[2 * e]
        return [f"{prefix}mull v4.{wide}, {a}.{HALVES[e]}, {b}.{HALVES[e]}",
                f"{prefix}mull2 v5.{wide}, {a}.{ARRANGEMENTS[e]}, "
                f"{b}.{ARRANGEMENTS[e]}",
                f"uzp2 {d}.{ARRANGEMENTS[e]}, v4.{ARRANGEMENTS[e]}, "
                f"v5.{ARRANGEMENTS[e]}"]
    return lines


def shift_by_vector(kind: str, reverse: bool):
    """LSL, LSR or ASR of each element by the unsigned amount in the other
    operand's element: an amount of the element size or more shifts every
    bit out (or fills with the sign), so it is clamped to the size before
    AdvSIMD's signed-byte shift reads it."""
    def lines(e, d, a, b):
        value, amount = (b, a) if reverse else (a, b)
        arrangement = ARRANGEMENTS[e]
        if e == 8:
            body = ["mov x9, #64", "dup v6.2d, x9",
                    f"cmhi v4.2d, {amount}.2d, v6.2d",
                    f"bsl v4.16b, v6.16b, {amount}.16b",
                    "mov v5.16b, v4.16b"]
        else:
            body = [f"movi v6.{arrangement}, #{8 * e}",
                    f"umin v5.{arrangement}, {amount}.{arrangement}, "
                    f"v6.{arrangement}"]
        if kind != "lsl":
            body.append(f"neg v5.{arrangement}, v5.{arrangement}")
        operation = "sshl" if kind == "asr" else "ushl"
        return body + [f"{operation} {d}.{arrangement}, {value}.{arrangement},"
                       f" v5.{arrangement}"]
    return lines


INTEGER_BINARY_PREDICATED = {
    "add_z_p_zz_": vector_op("add"),
    "sub_z_p_zz_": vector_op("sub"),
    "subr_z_p_zz_": lambda e, d, a, b: vector_op("sub")(e, d, b, a),
    "mul_z_p_zz_": doubleword_by(vector_op("mul"), lanes_in_gprs("mul")),
    "smulh_z_p_zz_": multiply_high_into(True),
    "umulh_z_p_zz_": multiply_high_into(False),
    "smax_z_p_zz_": doubleword_by(vector_op("smax"),
                                  select_by("cmgt", False)),
    "smin_z_p_zz_": doubleword_by(vector_op("smin"), select_by("cmgt", True)),
    "umax_z_p_zz_": doubleword_by(vector_op("umax"),
                                  select_by("cmhi", False)),
    "umin_z_p_zz_": doubleword_by(vector_op("umin"), select_by("cmhi", True)),
    "sabd_z_p_zz_": doubleword_by(vector_op("sabd"),
                                  absolute_difference("cmgt")),
    "uabd_z_p_zz_": doubleword_by(vector_op("uabd"),
                                  absolute_difference("cmhi")),
    "and_z_p_zz_": bitwise_op("and"),
    "orr_z_p_zz_": bitwise_op("orr"),
    "eor_z_p_zz_": bitwise_op("eor"),
    "bic_z_p_zz_": bitwise_op("bic"),
    "lsl_z_p_zz_": shift_by_vector("lsl", False),
    "lsr_z_p_zz_": shift_by_vector("lsr", False),
    "asr_z_p_zz_": shift_by_vector("asr", False),
    "lslr_z_p_zz_": shift_by_vector("lsl", True),
    "lsrr_z_p_zz_": shift_by_vector("lsr", True),
    "asrr_z_p_zz_": shift_by_vector("asr", True),
}


def integer_binary_predicated(operation):
    """Zdn := Zdn op Zm under Pg (merging): slots Zdn (x3), Zm (x4), Pg
    (x5)."""
    def lines(e: int) -> list:
        return ["ldr x8, [_stream], #8"] + unpack(3) + [
            "ldr q1, [_cpu, x3]", "ldr q2, [_cpu, x4]"] + \
            operation(e, "v3", "v1", "v2") + ["ldr q0, [_cpu, x5]"] + \
            lane_mask(e) + ["bit v1.16b, v3.16b, v0.16b",
                            "str q1, [_cpu, x3]", "next_gadget"]
    return lines


def integer_multiply_add(subtract: bool):
    """d := a + (or -) the product of the next two registers, which the
    caller names v0 (a), v1 and v2."""
    def lines(e, d):
        if e == 8:
            operation = "msub" if subtract else "madd"
            return ["fmov x9, d0", "fmov x10, d1", "fmov x11, d2",
                    f"{operation} x9, x10, x11, x9", "mov x12, v0.d[1]",
                    "mov x13, v1.d[1]", "mov x14, v2.d[1]",
                    f"{operation} x12, x13, x14, x12", f"fmov d{d[1:]}, x9",
                    f"mov {d}.d[1], x12"]
        arrangement = ARRANGEMENTS[e]
        return [f"mov {d}.16b, v0.16b",
                f"{'mls' if subtract else 'mla'} {d}.{arrangement}, "
                f"v1.{arrangement}, v2.{arrangement}"]
    return lines


def integer_ternary_predicated(subtract: bool, addend: int,
                               multiplicand: int, multiplier: int,
                               destination: int):
    """MLA, MLS, MAD or MSB under Pg (merging), the slots as for the
    floating-point forms."""
    def lines(e: int) -> list:
        original = "v0" if destination == addend else "v1"
        return ["ldr x8, [_stream], #8"] + unpack(4) + [
            f"ldr q0, [_cpu, x{3 + addend}]",
            f"ldr q1, [_cpu, x{3 + multiplicand}]",
            f"ldr q2, [_cpu, x{3 + multiplier}]"] + \
            integer_multiply_add(subtract)(e, "v3") + [
            "ldr q4, [_cpu, x6]"] + lane_mask(e, 4) + [
            f"bit {original}.16b, v3.16b, v4.16b",
            f"str q{original[1:]}, [_cpu, x{3 + destination}]",
            "next_gadget"]
    return lines


# name: (subtract, addend, multiplicand, multiplier, destination) over the
# slots (z, 0), (z, 5), (z, 16) and Pg.
INTEGER_TERNARY_PREDICATED = {
    "mla_z_p_zzz_": (False, 0, 1, 2, 0),
    "mls_z_p_zzz_": (True, 0, 1, 2, 0),
    "mad_z_p_zzz_": (False, 1, 0, 2, 0),
    "msb_z_p_zzz_": (True, 1, 0, 2, 0),
}


def extend(signed: bool, width: int):
    """SXT or UXT of the low `width` bytes of each element."""
    def lines(e, d, a):
        if e <= width:
            return ABSENT
        arrangement = ARRANGEMENTS[e]
        if signed:
            shift = 8 * (e - width)
            return [f"shl {d}.{arrangement}, {a}.{arrangement}, #{shift}",
                    f"sshr {d}.{arrangement}, {d}.{arrangement}, #{shift}"]
        mask = 0
        for byte in range(8):
            if byte % e < width:
                mask |= 0xff << (8 * byte)
        return [f"movi v6.2d, #0x{mask:016x}",
                f"and {d}.16b, {a}.16b, v6.16b"]
    return lines


def reverse_within(unit: int):
    """REVB, REVH or REVW: the units of `unit` bytes reversed within each
    element."""
    def lines(e, d, a):
        if e <= unit:
            return ABSENT
        return [f"rev{8 * e} {d}.{ARRANGEMENTS[unit]}, {a}.{ARRANGEMENTS[unit]}"]
    return lines


def count_bits(e, d, a):
    body = [f"cnt {d}.16b, {a}.16b"]
    for size in (2, 4, 8):
        if size <= e:
            body.append(f"uaddlp {d}.{ARRANGEMENTS[size]}, "
                        f"{d}.{ARRANGEMENTS[size // 2]}")
    return body


def reverse_bits(e, d, a):
    if e == 1:
        return [f"rbit {d}.16b, {a}.16b"]
    return [f"rev{8 * e} {d}.16b, {a}.16b", f"rbit {d}.16b, {d}.16b"]


INTEGER_UNARY_PREDICATED = {
    "abs_z_p_z_m": lambda e, d, a: [f"abs {d}.{ARRANGEMENTS[e]}, "
                                    f"{a}.{ARRANGEMENTS[e]}"],
    "neg_z_p_z_m": lambda e, d, a: [f"neg {d}.{ARRANGEMENTS[e]}, "
                                    f"{a}.{ARRANGEMENTS[e]}"],
    "not_z_p_z_m": lambda e, d, a: [f"not {d}.16b, {a}.16b"],
    "cnt_z_p_z_m": count_bits,
    "clz_z_p_z_m": lambda e, d, a: ABSENT if e == 8 else [
        f"clz {d}.{ARRANGEMENTS[e]}, {a}.{ARRANGEMENTS[e]}"],
    "cls_z_p_z_m": lambda e, d, a: ABSENT if e == 8 else [
        f"cls {d}.{ARRANGEMENTS[e]}, {a}.{ARRANGEMENTS[e]}"],
    "rbit_z_p_z_m": reverse_bits,
    "sxtb_z_p_z_m": extend(True, 1),
    "sxth_z_p_z_m": extend(True, 2),
    "sxtw_z_p_z_m": extend(True, 4),
    "uxtb_z_p_z_m": extend(False, 1),
    "uxth_z_p_z_m": extend(False, 2),
    "uxtw_z_p_z_m": extend(False, 4),
    "revb_z_z_m": reverse_within(1),
    "revh_z_z_m": reverse_within(2),
    "revw_z_z_m": reverse_within(4),
}


def integer_unary_predicated(operation):
    """Zd := op(Zn) under Pg (merging): slots Zd (x3), Zn (x4), Pg (x5)."""
    def lines(e: int):
        result = operation(e, "v3", "v1")
        if result is ABSENT:
            return ABSENT
        return ["ldr x8, [_stream], #8"] + unpack(3) + [
            "ldr q1, [_cpu, x4]"] + result + ["ldr q0, [_cpu, x5]"] + \
            lane_mask(e) + ["ldr q2, [_cpu, x3]",
                            "bit v2.16b, v3.16b, v0.16b",
                            "str q2, [_cpu, x3]", "next_gadget"]
    return lines


# Shifts by an immediate: one gadget per value of tsz:imm3, which fixes
# both the element size and the amount, so each runs AdvSIMD's own
# immediate form. A variant is UInt(tsz:imm3); tsz's highest set bit gives
# the element size of `esize` bits, a right shift is 2 * esize - variant
# and a left shift variant - esize.

UNPREDICATED_SHIFT_BITS = [16, 17, 18, 19, 20, 22, 23]
NARROWING_SHIFT_BITS = [16, 17, 18, 19, 20, 22]
PREDICATED_SHIFT_BITS = [5, 6, 7, 8, 9, 22, 23]


def shift_variant(value: int):
    """(element bytes, right amount, left amount) of a tsz:imm3 value, or
    None for the reserved tsz 0."""
    tsz = value >> 3
    if tsz == 0:
        return None
    e = 1 << (tsz.bit_length() - 1)
    bits = 8 * e
    return e, 2 * bits - value, value - bits


def two_register(lines: list, accumulate: bool = False) -> list:
    """Zd := f(Zn): slot 0 Zd in x3 (v0, holding Zd first when
    `accumulate`), slot 1 Zn in x4 (v1)."""
    body = ["ldr x8, [_stream], #8", "lsr x4, x8, #16",
            "ldr q1, [_cpu, x4]", "and x3, x8, #0xffff"]
    if accumulate:
        body.append("ldr q0, [_cpu, x3]")
    return body + lines + ["str q0, [_cpu, x3]", "next_gadget"]


def narrow_maximum(e: int) -> list:
    """v4 := the largest unsigned narrow value in each wide element of
    `e` bytes."""
    mask = 0
    for byte in range(8):
        if byte % e < e // 2:
            mask |= 0xff << (8 * byte)
    return [f"movi v4.2d, #0x{mask:016x}"]


def unsigned_minimum(e: int, d: str, a: str, b: str) -> list:
    if e == 8:
        return [f"cmhi v5.2d, {a}.2d, {b}.2d", f"mov {d}.16b, {a}.16b",
                f"bit {d}.16b, {b}.16b, v5.16b"]
    return [f"umin {d}.{ARRANGEMENTS[e]}, {a}.{ARRANGEMENTS[e]}, "
            f"{b}.{ARRANGEMENTS[e]}"]


def shift_immediate(kind: str):
    """The unpredicated and accumulating shifts of Zn, by kind: lines of
    (element bytes, right amount, left amount) that leave the result in
    v0, which holds Zd first for the accumulating kinds."""
    def lines(e: int, right: int, left: int):
        a = ARRANGEMENTS[e]
        bits = 8 * e
        if kind in ("lsl", "sli"):
            if not 0 <= left < bits:
                return ABSENT
            operation = "shl" if kind == "lsl" else "sli"
            return [f"{operation} v0.{a}, v1.{a}, #{left}"]
        if not 1 <= right <= bits:
            return ABSENT
        if kind == "xar":
            return ["eor v3.16b, v0.16b, v1.16b",
                    f"ushr v0.{a}, v3.{a}, #{right}",
                    f"sli v0.{a}, v3.{a}, #{bits - right}" if right < bits
                    else "mov v0.16b, v3.16b"]
        operation = {"lsr": "ushr", "asr": "sshr", "sri": "sri",
                     "usra": "usra", "ssra": "ssra", "ursra": "ursra",
                     "srsra": "srsra"}[kind]
        return [f"{operation} v0.{a}, v1.{a}, #{right}"]
    return lines


# name: (kind, accumulate)
SHIFT_IMMEDIATE = {
    "lsl_z_zi_": ("lsl", False),
    "lsr_z_zi_": ("lsr", False),
    "asr_z_zi_": ("asr", False),
    "sli_z_zzi_": ("sli", True),
    "sri_z_zzi_": ("sri", True),
    "usra_z_zi_": ("usra", True),
    "ssra_z_zi_": ("ssra", True),
    "ursra_z_zi_": ("ursra", True),
    "srsra_z_zi_": ("srsra", True),
    "xar_z_zzi_": ("xar", True),
}


def narrowing_shift(kind: str, top: bool):
    """SHRN, RSHRN, UQSHRN and UQRSHRN, bottom or top. The tsz value names
    the narrow element; the wide one is twice it. The result's narrow
    element sits in the low half of each wide element: the bottom form
    stores it with the top half zero, and the top form inserts it into
    Zd's top halves. Saturation is an unsigned minimum, since AdvSIMD's
    saturating forms set FPSR.QC, which SVE's never do."""
    def lines(narrow: int, right: int, left: int):
        e = 2 * narrow
        if e > 8 or not 1 <= right <= 8 * narrow:
            return ABSENT
        a = ARRANGEMENTS[e]
        rounding = kind in ("rshrn", "uqrshrn")
        body = [f"{'urshr' if rounding else 'ushr'} v3.{a}, v1.{a}, #{right}"]
        if kind.startswith("uq"):
            body += narrow_maximum(e) + unsigned_minimum(e, "v3", "v3", "v4")
        if top:
            return body + [f"sli v0.{a}, v3.{a}, #{8 * narrow}"]
        if not kind.startswith("uq"):
            body += narrow_maximum(e)
            body.append("and v3.16b, v3.16b, v4.16b")
        return body + ["mov v0.16b, v3.16b"]
    return lines


NARROWING_SHIFT = {
    "shrnb_z_zi_": ("shrn", False),
    "shrnt_z_zi_": ("shrn", True),
    "rshrnb_z_zi_": ("rshrn", False),
    "rshrnt_z_zi_": ("rshrn", True),
    "uqshrnb_z_zi_": ("uqshrn", False),
    "uqshrnt_z_zi_": ("uqshrn", True),
    "uqrshrnb_z_zi_": ("uqrshrn", False),
    "uqrshrnt_z_zi_": ("uqrshrn", True),
}


def predicated_shift(kind: str):
    """The predicated shifts of Zdn, merged under Pg: slots Zdn (x3), Pg
    (x4); the result in v3. UQSHL saturates where a set bit would shift
    out: the bits a left shift loses, shifted down, are nonzero."""
    def lines(e: int, right: int, left: int):
        a = ARRANGEMENTS[e]
        bits = 8 * e
        if kind in ("lsl", "uqshl"):
            if not 0 <= left < bits:
                return ABSENT
            if kind == "lsl":
                result = [f"shl v3.{a}, v1.{a}, #{left}"]
            else:
                result = [f"ushr v4.{a}, v1.{a}, #{bits - left}",
                          f"cmtst v4.{a}, v4.{a}, v4.{a}",
                          f"shl v3.{a}, v1.{a}, #{left}",
                          "orr v3.16b, v3.16b, v4.16b"]
        else:
            if not 1 <= right <= bits:
                return ABSENT
            operation = {"lsr": "ushr", "asr": "sshr", "urshr": "urshr",
                         "srshr": "srshr"}[kind]
            result = [f"{operation} v3.{a}, v1.{a}, #{right}"]
        return ["ldr x8, [_stream], #8", "and x3, x8, #0xffff",
                "lsr x4, x8, #16", "ldr q1, [_cpu, x3]"] + result + [
                "ldr q0, [_cpu, x4]"] + lane_mask(e) + [
                "bit v1.16b, v3.16b, v0.16b", "str q1, [_cpu, x3]",
                "next_gadget"]
    return lines


PREDICATED_SHIFT = {
    "lsl_z_p_zi_": "lsl",
    "lsr_z_p_zi_": "lsr",
    "asr_z_p_zi_": "asr",
    "uqshl_z_p_zi_": "uqshl",
    "urshr_z_p_zi_": "urshr",
    "srshr_z_p_zi_": "srshr",
}


def by_shift_variant(form: Form, bits: list, template) -> None:
    form.variant_bits = bits
    form.variants = []
    for value in range(1 << len(bits)):
        decoded = shift_variant(value)
        form.variants.append(ABSENT if decoded is None else
                             template(*decoded))


# Unpredicated forms of Zdn and an immediate the builder replicates across
# the stream's doubleword: slot Zdn in x8, v1 := Zdn, v2 := the immediate,
# and the lines leave the result in v0. name: (lowering, lines by size).

def immediate_form(lines: list) -> list:
    return ["ldp x8, x9, [_stream], #16", "dup v2.2d, x9",
            "ldr q1, [_cpu, x8]"] + lines + ["str q0, [_cpu, x8]",
                                             "next_gadget"]


def into_v0(operation):
    """An operation of the predicated forms' shape, d := op(a, b), on
    v0 := op(v1, v2)."""
    return lambda e: operation(e, "v0", "v1", "v2")


IMMEDIATE_FORMS = {
    "add_z_zi_": ("unsigned_immediate", into_v0(vector_op("add"))),
    "sub_z_zi_": ("unsigned_immediate", into_v0(vector_op("sub"))),
    "subr_z_zi_": ("unsigned_immediate",
                   lambda e: vector_op("sub")(e, "v0", "v2", "v1")),
    "uqadd_z_zi_": ("unsigned_immediate", unsigned_saturating_add),
    "uqsub_z_zi_": ("unsigned_immediate", unsigned_saturating_subtract),
    "smax_z_zi_": ("integer_immediate", into_v0(
        doubleword_by(vector_op("smax"), select_by("cmgt", False)))),
    "smin_z_zi_": ("integer_immediate", into_v0(
        doubleword_by(vector_op("smin"), select_by("cmgt", True)))),
    "umax_z_zi_": ("unsigned_immediate", into_v0(
        doubleword_by(vector_op("umax"), select_by("cmhi", False)))),
    "umin_z_zi_": ("unsigned_immediate", into_v0(
        doubleword_by(vector_op("umin"), select_by("cmhi", True)))),
    "mul_z_zi_": ("integer_immediate", into_v0(
        doubleword_by(vector_op("mul"), lanes_in_gprs("mul")))),
}

# The logical immediates, whose bitmask is a doubleword already.
LOGICAL_IMMEDIATE = {"and_z_zi_": "and", "orr_z_zi_": "orr",
                     "eor_z_zi_": "eor"}


def dot_product(prefix: str, indexed: bool):
    """SDOT and UDOT: 32-bit sums of four byte products by AdvSIMD's own
    form, or 64-bit sums of four halfword products, which AdvSIMD lacks,
    from the widening products added pairwise twice. An indexed form's
    variant is its index, a 32-bit group or for halfwords a 64-bit one."""
    def doubleword(index) -> list:
        source = "v2"
        lines = []
        if index is not None:
            lines.append(f"dup v5.2d, v2.d[{index}]")
            source = "v5"
        return lines + [
            f"{prefix}mull v3.4s, v1.4h, {source}.4h",
            f"{prefix}mull2 v4.4s, v1.8h, {source}.8h",
            f"{prefix}addlp v3.2d, v3.4s", f"{prefix}addlp v4.2d, v4.4s",
            "addp v3.2d, v3.2d, v4.2d", "add v0.2d, v0.2d, v3.2d"]
    return doubleword if indexed else lambda size: (
        [f"{prefix}dot v0.4s, v1.16b, v2.16b"] if size == 0 else
        doubleword(None))


COMPARISONS = {"cmpeq_p_p_zz_": ("cmeq", False),
               "cmpne_p_p_zz_": ("cmeq", True),
               "cmpgt_p_p_zz_": ("cmgt", False),
               "cmpge_p_p_zz_": ("cmge", False),
               "cmphi_p_p_zz_": ("cmhi", False),
               "cmphs_p_p_zz_": ("cmhs", False)}


def comparison(operation: str, invert: bool):
    """Pd := Pg AND (Zn cmp Zm), one active byte per element, with NZCV
    from PredTest over Pg's elements: slots Pd (x3), Zn (x4), Zm (x5), Pg
    (x6)."""
    def lines(e: int) -> list:
        a = ARRANGEMENTS[e]
        body = ["ldr x8, [_stream], #8"] + unpack(4) + [
            "ldr q1, [_cpu, x4]", "ldr q2, [_cpu, x5]", "ldr q0, [_cpu, x6]",
            f"{operation} v3.{a}, v1.{a}, v2.{a}"]
        if invert:
            body.append("not v3.16b, v3.16b")
        if e > 1:
            body += [f"sve_element_pattern {e}, 7",
                     "and v0.16b, v0.16b, v7.16b"]
        return body + ["and v3.16b, v3.16b, v0.16b", "str q3, [_cpu, x3]"] + \
            predicate_test("v0", "v3") + ["next_gadget"]
    return lines


# Contiguous memory. A load or store whose footprint lies in one guest page
# the TLB holds runs on the host address: a load reads the whole footprint,
# which on a mapped page is harmless for inactive elements, and zeroes
# those by the lane mask; a store runs only when its predicate is all-true,
# as one store of the footprint. Everything else, a crossing, a page the
# TLB cannot fill, a fault, a partial store, a tagged address, goes to the
# semantic library through sve_library, which decides faults and partial
# writes. Stream: [packed slots][byte offset][library operand][PC]: the
# library operand is the word and row of an sve_library stream, so the slow
# path is a branch to that gadget with _stream at it. Slot offsets live in
# x19 (Zt) and x20 (Pg), which a TLB miss's helper call preserves.

MEMORY_SHAPES = ("contiguous", "replicate", "replicate_quad", "structure",
                 "fill_z")
MEMORY_OFFSETS = ("immediate_footprint", "register_element",
                  "immediate_element", "immediate_16")
LABELS = itertools.count()


def memory_footprint(memory, e: int, m: int) -> int:
    if memory.shape == "replicate":
        return m
    if memory.shape in ("replicate_quad", "fill_z"):
        return 16
    return memory.registers * (16 // e) * m


def memory_gadget(memory, e: int, m: int, predicated: bool,
                  indexed: bool) -> list:
    """One variant of a contiguous load or store of elements of `e` bytes
    from memory elements of `m` bytes."""
    number = next(LABELS)
    cold, resume, slow, tagged, translate = (
        f"Lsve_memory_{number}_{name}"
        for name in ("cold", "resume", "slow", "tagged", "translate"))
    load = memory.direction == "load"
    footprint = memory_footprint(memory, e, m)
    registers = memory.registers
    base_slot = 2 if predicated else 1
    page = "UML_GADGET_TLB_ENTRY_READ_PAGE" if load else \
        "UML_GADGET_TLB_ENTRY_WRITE_PAGE"
    body = ["ldp x8, x9, [_stream], #16", "and x19, x8, #0xffff"]
    if predicated:
        body.append("ubfx x20, x8, #16, #16")
    body += [f"ubfx x10, x8, #{16 * base_slot}, #16", "ldr x7, [_cpu, x10]"]
    if indexed:
        body += [f"lsr x11, x8, #{16 * (base_slot + 1)}",
                 "ldr x11, [_cpu, x11]",
                 f"add x7, x7, x11, lsl #{m.bit_length() - 1}"]
    else:
        body.append("add x7, x7, x9")
    if predicated and not load:
        test = all_true_test(e, "x20")
        test[-1] = test[-1].replace("1f", slow)
        body += test
    body += ["ldrb w10, [_cpu, #UML_GADGET_CPU_TBI_ENABLED]",
             f"cbnz w10, {tagged}", f"{translate}:",
             f"transfer_translate {int(not load)}, {page}, {footprint}, x7, "
             f"{cold}, {resume}, {slow}"]
    a = ARRANGEMENTS[e]
    width = {1: "b", 2: "h", 4: "s", 8: "d", 16: "q"}
    if load:
        if memory.shape == "replicate":
            if m == 8:
                instruction, target = "ldr", "x9"
            elif memory.sign_extend:
                instruction = {1: "ldrsb", 2: "ldrsh", 4: "ldrsw"}[m]
                target = "x9" if e == 8 else "w9"
            else:
                instruction = {1: "ldrb", 2: "ldrh", 4: "ldr"}[m]
                target = "w9"
            body += [f"{instruction} {target}, [x7]",
                     f"dup v0.{a}, {'x9' if e == 8 else 'w9'}"]
        elif memory.shape == "structure":
            names = ", ".join(f"v{k}.{a}" for k in range(registers))
            body.append(f"ld{registers} {{{names}}}, [x7]")
        else:
            body.append(f"ldr {width[footprint]}0, [x7]")
            size = m
            while size < e:
                extend = "sxtl" if memory.sign_extend else "uxtl"
                body.append(f"{extend} v0.{ARRANGEMENTS[2 * size]}, "
                            f"v0.{HALVES[size]}")
                size *= 2
        if predicated:
            body += ["ldr q6, [_cpu, x20]"] + lane_mask(e, 6)
            body += [f"and v{k}.16b, v{k}.16b, v6.16b"
                     for k in range(registers)]
        body.append("str q0, [_cpu, x19]")
        for k in range(1, registers):
            body += ["sub x10, x19, #UML_GADGET_CPU_V0",
                     f"add x10, x10, #{16 * k}", "and x10, x10, #0x1f0",
                     "add x10, x10, #UML_GADGET_CPU_V0",
                     f"str q{k}, [_cpu, x10]"]
    else:
        body.append("ldr q0, [_cpu, x19]")
        for k in range(1, registers):
            body += ["sub x10, x19, #UML_GADGET_CPU_V0",
                     f"add x10, x10, #{16 * k}", "and x10, x10, #0x1f0",
                     "add x10, x10, #UML_GADGET_CPU_V0",
                     f"ldr q{k}, [_cpu, x10]"]
        if memory.shape == "structure":
            names = ", ".join(f"v{k}.{a}" for k in range(registers))
            body.append(f"st{registers} {{{names}}}, [x7]")
        else:
            size = e
            while size > m:
                body.append(f"xtn v0.{HALVES[size // 2]}, "
                            f"v0.{ARRANGEMENTS[size]}")
                size //= 2
            body.append(f"str {width[footprint]}0, [x7]")
    body += ["add _stream, _stream, #16", "next_gadget",
             f"{tagged}:", "ubfx x7, x7, #0, #56", f"b {translate}",
             f"transfer_translate_cold {int(not load)}, {page}, {footprint}, "
             f"x7, {cold}, {resume}, {slow}, 0, 0, {slow}",
             f"{slow}:", "b NAME(uml_arm64_gadget_sve_library)"]
    return body


def memory_form(row):
    """The slots and a variant builder for a contiguous memory row, or
    None for a row the semantic library runs."""
    memory = row.memory
    if memory.shape not in MEMORY_SHAPES or \
            memory.offset not in MEMORY_OFFSETS:
        return None
    kinds = [operand.kind for operand in row.operands]
    predicated = "p" in kinds
    indexed = memory.offset == "register_element"
    wanted = [("z", 0)] + ([("p", 10)] if predicated else []) + \
        [("gpr_or_sp", 5)] + ([("gpr", 16)] if indexed else [])
    m = memory.msize // 8

    def build(form: Form) -> None:
        if memory.esize_shift is None:
            form.variants = [memory_gadget(memory, memory.esize // 8, m,
                                           predicated, indexed)]
            return
        segment = memory.esize_shift
        form.variant_bits = list(range(segment.low,
                                       segment.low + segment.width))
        for value in range(1 << segment.width):
            e = (memory.esize // 8) << value
            form.variants.append(
                ABSENT if e < m or e > 8 else
                memory_gadget(memory, e, m, predicated, indexed))
    return tuple(wanted), build


# Form construction.

def sized(form: Form, template) -> None:
    """One variant per element size; a size the template returns ABSENT
    for runs through the semantic library."""
    form.variant_bits = list(SIZE_BITS)
    form.variants = [template(esize) for esize in ESIZES]


def single(form: Form, lines: list) -> None:
    form.variants = [lines]


def build_forms(table) -> list:
    forms: list[Form] = []
    by_name: dict[str, Form] = {}

    def add(row, lowering: str, wanted: tuple, build=None, key: str = "",
            argument: int = 0, flags: int = 0) -> None:
        name = row.encoding
        if name in by_name:
            by_name[name].rows.append(row.index)
            return
        form = Form(name, [row.index], lowering, slots_of(row, *wanted),
                    key=key or name, argument=argument, flags=flags)
        if build is not None:
            build(form)
        by_name[name] = form
        forms.append(form)

    for row in table.rows:
        if "sve" not in row.non_streaming_requirement:
            continue
        name = row.encoding
        base = name.split("_")[0]
        if row.memory is not None:
            if row.memory.shape == "prefetch":
                # A hint, which never faults: the base part's NOP.
                if name not in by_name:
                    form = Form(name, [row.index], "prefetch", [], key=name)
                    by_name[name] = form
                    forms.append(form)
                else:
                    by_name[name].rows.append(row.index)
                continue
            described = memory_form(row)
            if described is not None:
                add(row, "memory", described[0], described[1])
            continue
        if name in IMMEDIATE_FORMS:
            lowering, lines = IMMEDIATE_FORMS[name]
            add(row, lowering, (("z", 0),),
                lambda f, l=lines: sized(f, lambda e: immediate_form(l(e))))
        elif name in LOGICAL_IMMEDIATE:
            add(row, "bitmask_immediate", (("z", 0),),
                lambda f, o=LOGICAL_IMMEDIATE[name]: single(
                    f, immediate_form([f"{o} v0.16b, v1.16b, v2.16b"])))
        elif name in ("sdot_z_zzz_", "udot_z_zzz_"):
            def dots(form: Form, prefix=base[0]) -> None:
                form.variant_bits = [22]
                form.variants = [three_register(dot_product(prefix, False)(
                    size), accumulate=True) for size in (0, 1)]
            add(row, "plain", (("z", 0), ("z", 5), ("z", 16)), dots)
        elif name in ("sdot_z_zzzi_s", "udot_z_zzzi_s", "usdot_z_zzzi_s",
                      "sudot_z_zzzi_s"):
            def dots(form: Form, operation=base) -> None:
                form.variant_bits = [19, 20]
                form.variants = [three_register([
                    f"{operation} v0.4s, v1.16b, v2.4b[{index}]"],
                    accumulate=True) for index in range(4)]
            add(row, "plain", (("z", 0), ("z", 5), ("z", 16)), dots)
        elif name in ("sdot_z_zzzi_d", "udot_z_zzzi_d"):
            def dots(form: Form, prefix=base[0]) -> None:
                form.variant_bits = [20]
                form.variants = [three_register(dot_product(prefix, True)(
                    index), accumulate=True) for index in range(2)]
            add(row, "plain", (("z", 0), ("z", 5), ("z", 16)), dots)
        elif name == "usdot_z_zzz_s":
            add(row, "plain", (("z", 0), ("z", 5), ("z", 16)),
                lambda f: single(f, three_register(
                    ["usdot v0.4s, v1.16b, v2.16b"], accumulate=True)))
        elif name in COMPARISONS:
            add(row, "plain", (("p", 0), ("z", 5), ("z", 16), ("p", 10)),
                lambda f, c=COMPARISONS[name]: sized(f, comparison(*c)))
        elif name in SHIFT_IMMEDIATE:
            kind, accumulate = SHIFT_IMMEDIATE[name]

            def shifts(form: Form, kind=kind, accumulate=accumulate):
                body = shift_immediate(kind)
                by_shift_variant(form, UNPREDICATED_SHIFT_BITS,
                                 lambda *v: None if body(*v) is ABSENT else
                                 two_register(body(*v), accumulate))
            add(row, "plain", (("z", 0), ("z", 5)), shifts)
        elif name in NARROWING_SHIFT:
            kind, top = NARROWING_SHIFT[name]

            def narrowing(form: Form, kind=kind, top=top):
                body = narrowing_shift(kind, top)
                by_shift_variant(form, NARROWING_SHIFT_BITS,
                                 lambda *v: None if body(*v) is ABSENT else
                                 two_register(body(*v), top))
            add(row, "plain", (("z", 0), ("z", 5)), narrowing)
        elif name in PREDICATED_SHIFT:
            add(row, "plain", (("z", 0), ("p", 10)),
                lambda f, k=PREDICATED_SHIFT[name]: by_shift_variant(
                    f, PREDICATED_SHIFT_BITS, predicated_shift(k)))
        elif name in INTEGER_BINARY_PREDICATED:
            add(row, "plain", (("z", 0), ("z", 5), ("p", 10)),
                lambda f, o=INTEGER_BINARY_PREDICATED[name]: sized(
                    f, integer_binary_predicated(o)))
        elif name in INTEGER_TERNARY_PREDICATED:
            add(row, "plain", (("z", 0), ("z", 5), ("z", 16), ("p", 10)),
                lambda f, a=INTEGER_TERNARY_PREDICATED[name]: sized(
                    f, integer_ternary_predicated(*a)))
        elif name in INTEGER_UNARY_PREDICATED:
            add(row, "plain", (("z", 0), ("z", 5), ("p", 10)),
                lambda f, o=INTEGER_UNARY_PREDICATED[name]: sized(
                    f, integer_unary_predicated(o)))
        elif name in FP_BINARY_PREDICATED:
            add(row, "plain", (("z", 0), ("z", 5), ("p", 10)),
                lambda f, l=FP_BINARY_PREDICATED[name]: sized(f, l))
        elif name in FP_TERNARY_PREDICATED:
            operation, negate, addend, multiplicand, multiplier, \
                destination = FP_TERNARY_PREDICATED[name]
            # fmla-like rows name Zda, Zn, Zm; fmad-like Zdn, Zm, Za.
            wanted = (("z", 0), ("z", 5), ("z", 16), ("p", 10))
            add(row, "plain", wanted,
                lambda f, l=fp_ternary_predicated(
                    operation, negate, addend, multiplicand, multiplier,
                    destination): sized(f, l))
        elif name in FP_UNARY_PREDICATED:
            add(row, "plain", (("z", 0), ("z", 5), ("p", 10)),
                lambda f, l=FP_UNARY_PREDICATED[name]: sized(f, l))
        elif name in FP_INDEXED:
            operation, letter, bits = FP_INDEXED[name]
            arrangement = {"h": "8h", "s": "4s", "d": "2d"}[letter]

            def indexed(form: Form, operation=operation, letter=letter,
                        bits=bits, arrangement=arrangement) -> None:
                form.variant_bits = bits
                form.variants = [three_register([
                    f"{operation} v0.{arrangement}, v1.{arrangement}, "
                    f"v2.{letter}[{index}]"],
                    accumulate=operation != "fmul", fp=True)
                    for index in range(1 << len(bits))]
            add(row, "plain", (("z", 0), ("z", 5), ("z", 16)), indexed)
        elif name in THREE_REGISTER:
            lines, accumulate, fp = THREE_REGISTER[name]
            add(row, "plain", (("z", 0), ("z", 5), ("z", 16)),
                lambda f, l=lines, a=accumulate, p=fp: sized(
                    f, lambda e: None if l(e) is ABSENT else
                    three_register(l(e), a, p)))
        elif name == "pfalse_p_":
            add(row, "plain", (("p", 0),), lambda f: single(f, [
                "ldr x8, [_stream], #8", "add x8, _cpu, x8",
                "stp xzr, xzr, [x8]", "next_gadget"]))
        elif name in ("ptrue_p_s_", "ptrues_p_s_"):
            flags = name == "ptrues_p_s_"
            lines = ["ldp x8, x9, [_stream], #16"]
            lines.append("ldp x10, x27, [_stream], #16" if flags else
                         "ldr x10, [_stream], #8")
            lines += ["add x8, _cpu, x8", "stp x9, x10, [x8]", "next_gadget"]
            add(row, "predicate_constant_flags" if flags else
                "predicate_constant", (("p", 0),),
                lambda f, lines=lines: single(f, lines))
        elif name == "ptest__p_p_":
            add(row, "plain", (("p", 10), ("p", 5)), lambda f: single(f, [
                "ldr x8, [_stream], #8"] + unpack(2) + [
                "add x3, _cpu, x3", "add x4, _cpu, x4",
                "ldp x9, x10, [x3]", "ldp x11, x12, [x4]",
                "sve_predicate_test", "next_gadget"]))
        elif name.endswith("_p_p_pp_z") or name == "sel_p_p_pp_":
            operation = base[:-1] if base.endswith("s") and \
                base[:-1] in PREDICATE_LOGICAL else base
            flags = operation != base
            add(row, "plain", (("p", 0), ("p", 5), ("p", 16), ("p", 10)),
                lambda f, o=operation, s=flags: single(
                    f, predicate_logical(o, s) + ["next_gadget"]))
        elif name == "cntp_r_p_p_":
            add(row, "plain", (("gpr", 0), ("p", 10), ("p", 5)),
                lambda f: sized(f, lambda e: [
                    "ldr x8, [_stream], #8"] + unpack(3, skip=(0,)) + [
                    "ldr q0, [_cpu, x4]", "ldr q1, [_cpu, x5]",
                    "and v0.16b, v0.16b, v1.16b"] + active_count(e) + [
                    "and x3, x8, #0xffff", "str x9, [_cpu, x3]",
                    "next_gadget"]), flags=FLAG_DISCARD_ON_ZERO)
        elif name in ("incp_r_p_r_", "decp_r_p_r_"):
            operation = "add" if base == "incp" else "sub"
            add(row, "plain", (("gpr", 0), ("p", 5)),
                lambda f, o=operation: sized(f, lambda e: [
                    "ldr x8, [_stream], #8"] + unpack(2, skip=(0,)) + [
                    "ldr q0, [_cpu, x4]"] + active_count(e) + [
                    "and x3, x8, #0xffff", "ldr x10, [_cpu, x3]",
                    f"{o} x10, x10, x9", "str x10, [_cpu, x3]",
                    "next_gadget"]), flags=FLAG_DISCARD_ON_ZERO)
        elif name in ("incp_z_p_z_", "decp_z_p_z_"):
            operation = "add" if base == "incp" else "sub"
            add(row, "plain", (("z", 0), ("p", 5)),
                lambda f, o=operation: sized(f, lambda e: [
                    "ldr x8, [_stream], #8"] + unpack(2, skip=(0,)) + [
                    "ldr q0, [_cpu, x4]"] + active_count(e) + [
                    "and x3, x8, #0xffff", "ldr q1, [_cpu, x3]",
                    f"dup v2.{ARRANGEMENTS[e]}, {gpr(e, 9)}",
                    f"{o} v1.{ARRANGEMENTS[e]}, v1.{ARRANGEMENTS[e]}, "
                    f"v2.{ARRANGEMENTS[e]}",
                    "str q1, [_cpu, x3]", "next_gadget"]))
        elif name in ("cntb_r_s_", "cnth_r_s_", "cntw_r_s_", "cntd_r_s_",
                      "rdvl_r_i_"):
            add(row, "count_move", (("gpr", 0),),
                argument=1 if base == "rdvl" else 0)
        elif base[:3] in ("inc", "dec") and name.endswith("_r_rs_"):
            add(row, "count_add", (("gpr", 0),),
                argument=1 if base.startswith("dec") else 0)
        elif base[:3] in ("inc", "dec") and name.endswith("_z_zs_"):
            esize = ESIZES[row.value >> 22 & 3]
            operation = "add" if base.startswith("inc") else "sub"
            arrangement = ARRANGEMENTS[esize]
            add(row, "vector_count", (("z", 0),), lambda f: single(f, [
                "ldp x8, x9, [_stream], #16", "ldr q1, [_cpu, x8]",
                f"dup v2.{arrangement}, {gpr(esize, 9)}",
                f"{operation} v1.{arrangement}, v1.{arrangement}, "
                f"v2.{arrangement}",
                "str q1, [_cpu, x8]", "next_gadget"]))
        elif name in ("addvl_r_ri_", "addpl_r_ri_"):
            add(row, "vector_length_add",
                (("gpr_or_sp", 0), ("gpr_or_sp", 16)),
                argument=1 if name.startswith("addpl") else 0)
        elif name == "setffr_f_":
            form = Form(name, [row.index], "plain", [], key=name)
            single(form, ["mov x9, #-1",
                          "add x10, _cpu, #UML_ARM64_CPU_SVE_FFR",
                          "stp x9, x9, [x10]", "next_gadget"])
            by_name[name] = form
            forms.append(form)
        elif name == "rdffr_p_f_":
            add(row, "plain", (("p", 0),), lambda f: single(f, [
                "ldr x8, [_stream], #8",
                "add x10, _cpu, #UML_ARM64_CPU_SVE_FFR", "ldr q0, [x10]",
                "str q0, [_cpu, x8]", "next_gadget"]))
        elif name in ("rdffr_p_p_f_", "rdffrs_p_p_f_"):
            flags = name.startswith("rdffrs")
            add(row, "plain", (("p", 0), ("p", 5)),
                lambda f, s=flags: single(f, [
                    "ldr x8, [_stream], #8"] + unpack(2, skip=(0,)) + [
                    "ldr q0, [_cpu, x4]",
                    "add x10, _cpu, #UML_ARM64_CPU_SVE_FFR", "ldr q1, [x10]",
                    "and v1.16b, v1.16b, v0.16b", "and x3, x8, #0xffff",
                    "str q1, [_cpu, x3]"] +
                    (predicate_test("v0", "v1") if s else []) +
                    ["next_gadget"]))
        elif name == "wrffr_f_p_":
            add(row, "plain", (("p", 5),), lambda f: single(f, [
                "ldr x8, [_stream], #8", "ldr q0, [_cpu, x8]",
                "add x10, _cpu, #UML_ARM64_CPU_SVE_FFR", "str q0, [x10]",
                "next_gadget"]))
        elif name == "movprfx_z_z_":
            add(row, "plain", (("z", 0), ("z", 5)), lambda f: single(f, [
                "ldr x8, [_stream], #8"] + unpack(2, skip=(0,)) + [
                "ldr q0, [_cpu, x4]", "and x3, x8, #0xffff",
                "str q0, [_cpu, x3]", "next_gadget"]))
        elif name == "movprfx_z_p_z_":
            def movprfx(form: Form) -> None:
                form.variant_bits = [16] + SIZE_BITS
                for size in range(4):
                    for merging in (False, True):
                        esize = ESIZES[size]
                        lines = ["ldr x8, [_stream], #8"] + \
                            unpack(3, skip=(0,)) + ["ldr q0, [_cpu, x5]"] + \
                            lane_mask(esize) + ["ldr q1, [_cpu, x4]",
                                                "and x3, x8, #0xffff"]
                        if merging:
                            lines += ["ldr q2, [_cpu, x3]",
                                      "bit v2.16b, v1.16b, v0.16b",
                                      "str q2, [_cpu, x3]"]
                        else:
                            lines += ["and v1.16b, v1.16b, v0.16b",
                                      "str q1, [_cpu, x3]"]
                        form.variants.append(lines + ["next_gadget"])
            add(row, "plain", (("z", 0), ("z", 5), ("p", 10)), movprfx)
        elif name == "dup_z_r_":
            add(row, "plain", (("z", 0), ("gpr_or_sp", 5)),
                lambda f: sized(f, lambda e: [
                    "ldr x8, [_stream], #8"] + unpack(2, skip=(0,)) + [
                    "ldr x9, [_cpu, x4]",
                    f"dup v0.{ARRANGEMENTS[e]}, {gpr(e, 9)}",
                    "and x3, x8, #0xffff", "str q0, [_cpu, x3]",
                    "next_gadget"]))
        elif name in ("dup_z_i_", "dupm_z_i_", "fdup_z_i_"):
            lowering = {"dup": "integer_immediate", "dupm":
                        "bitmask_immediate", "fdup": "float_immediate"}[base]
            add(row, lowering, (("z", 0),), lambda f: single(f, [
                "ldp x8, x9, [_stream], #16", "dup v0.2d, x9",
                "str q0, [_cpu, x8]", "next_gadget"]), key="replicate")
        elif name in ("cpy_z_p_i_", "cpy_z_o_i_", "fcpy_z_p_i_"):
            merging = name != "cpy_z_o_i_"
            lowering = "float_immediate" if base == "fcpy" else \
                "integer_immediate"
            add(row, lowering, (("z", 0), ("p", 16)),
                lambda f, m=merging: sized(f, lambda e: [
                    "ldp x8, x9, [_stream], #16", "lsr x4, x8, #16",
                    "ldr q0, [_cpu, x4]"] + lane_mask(e) + [
                    "dup v1.2d, x9", "and x3, x8, #0xffff"] + (
                    ["ldr q2, [_cpu, x3]", "bit v2.16b, v1.16b, v0.16b",
                     "str q2, [_cpu, x3]"] if m else
                    ["and v1.16b, v1.16b, v0.16b", "str q1, [_cpu, x3]"]) +
                    ["next_gadget"]),
                key="cpy_immediate_" + ("merging" if merging else "zeroing"))
        elif name in ("cpy_z_p_r_", "cpy_z_p_v_"):
            source = "gpr_or_sp" if name == "cpy_z_p_r_" else "v"

            def copy(e: int, general: bool = source == "gpr_or_sp") -> list:
                if general:
                    load = ["ldr x9, [_cpu, x4]",
                            f"dup v1.{ARRANGEMENTS[e]}, {gpr(e, 9)}"]
                else:
                    letter = ARRANGEMENTS[e][-1]
                    load = ["ldr q1, [_cpu, x4]",
                            f"dup v1.{ARRANGEMENTS[e]}, v1.{letter}[0]"]
                return ["ldr x8, [_stream], #8"] + unpack(3, skip=(0,)) + [
                    "ldr q0, [_cpu, x5]"] + lane_mask(e) + load + [
                    "and x3, x8, #0xffff", "ldr q2, [_cpu, x3]",
                    "bit v2.16b, v1.16b, v0.16b", "str q2, [_cpu, x3]",
                    "next_gadget"]
            add(row, "plain", (("z", 0), (source, 5), ("p", 10)),
                lambda f, c=copy: sized(f, c))
        elif name == "dup_z_zi_":
            add(row, "index_table", (("z", 0), ("z", 5)), lambda f: single(f, [
                "ldr x8, [_stream], #8", "ldr q1, [_stream], #16",
                "lsr x4, x8, #16", "ldr q0, [_cpu, x4]",
                "tbl v0.16b, {v0.16b}, v1.16b", "and x3, x8, #0xffff",
                "str q0, [_cpu, x3]", "next_gadget"]))
        elif name == "sel_z_p_zz_":
            add(row, "plain", (("z", 0), ("z", 5), ("z", 16), ("p", 10)),
                lambda f: sized(f, lambda e: [
                    "ldr x8, [_stream], #8"] + unpack(4, skip=(0,)) + [
                    "ldr q0, [_cpu, x6]"] + lane_mask(e) + [
                    "ldr q1, [_cpu, x4]", "ldr q2, [_cpu, x5]",
                    "bsl v0.16b, v1.16b, v2.16b", "and x3, x8, #0xffff",
                    "str q0, [_cpu, x3]", "next_gadget"]))
        elif name in ("and_z_zz_", "orr_z_zz_", "eor_z_zz_", "bic_z_zz_"):
            add(row, "plain", (("z", 0), ("z", 5), ("z", 16)),
                lambda f, o=base: single(f, [
                    "ldr x8, [_stream], #8"] + unpack(3, skip=(0,)) + [
                    "ldr q0, [_cpu, x4]", "ldr q1, [_cpu, x5]",
                    f"{o} v0.16b, v0.16b, v1.16b", "and x3, x8, #0xffff",
                    "str q0, [_cpu, x3]", "next_gadget"]))
        elif base in ("whilelt", "whilele", "whilelo", "whilels", "whilegt",
                      "whilege", "whilehi", "whilehs") and \
                name.endswith("_p_p_rr_"):
            decrement = base[5:7] in ("gt", "ge", "hi", "hs")

            def whiles(form: Form, operation=base) -> None:
                form.variant_bits = [SF_BIT]
                form.variants = [while_compare(operation, sf, None)
                                 for sf in (0, 1)]
                form.variants += [while_compare(operation, sf, condition)
                                  for condition in FUSED_CONDITIONS
                                  for sf in (0, 1)]
            add(row, "while_compare", (("p", 0), ("gpr", 5), ("gpr", 16)),
                whiles, argument=int(decrement))
        elif name in ("whilewr_p_rr_", "whilerw_p_rr_"):
            add(row, "while_conflict", (("p", 0), ("gpr", 5), ("gpr", 16)),
                lambda f, o=base: sized(f, lambda e: while_conflict(o, e)))
    return forms


def assign_entries(forms: list) -> list:
    """Each form's first entry; forms with one key share entries."""
    shared: dict[str, Form] = {}
    labels = []
    for form in forms:
        if not form.variants:
            continue
        if form.key in shared:
            owner = shared[form.key]
            if owner.variants != form.variants or \
                    owner.variant_bits != form.variant_bits:
                raise GenerationError(f"{form.name}: shares {owner.name}'s "
                                      "entries but not its gadgets")
            form.first_entry = owner.first_entry
            continue
        shared[form.key] = form
        form.first_entry = len(labels)
        labels.extend((form, index) for index in range(len(form.variants)))
    return labels


def render_assembly(labels: list) -> str:
    out = ["/* SPDX-License-Identifier: GPL-2.0-or-later */",
           "/* Generated by darwin/cpu/arm64/gadgets/engine/"
           "generate_sve.py. Do not edit. */",
           "\t.p2align 4",
           "\t.globl NAME(uml_arm64_gadget_sve_generated_begin)",
           "NAME(uml_arm64_gadget_sve_generated_begin):"]
    names = []
    for number, (form, index) in enumerate(labels):
        if index == 0:
            out.append(f"/* {form.key} */")
        label = f"Lsve_generated_{number}"
        if form.variants[index] is ABSENT:
            names.append("NAME(uml_arm64_gadget_sve_generated_absent)")
            continue
        names.append(label)
        out += ["\t.p2align 4", f"{label}:"]
        out += [line if line.endswith(":") else "\t" + line
                for line in form.variants[index]]
    # The entry of every absent variant: the builder runs those words
    # through the semantic library, so nothing branches here.
    out += ["\t.p2align 4",
            "\t.globl NAME(uml_arm64_gadget_sve_generated_absent)",
            "NAME(uml_arm64_gadget_sve_generated_absent):",
            "\tbrk #0x5e5"]
    out += ["\t.p2align 4",
            "\t.globl NAME(uml_arm64_gadget_sve_generated_end)",
            "NAME(uml_arm64_gadget_sve_generated_end):",
            "",
            ".pushsection __DATA_CONST,__gadget_sve",
            "\t.p2align 3",
            "\t.globl NAME(uml_arm64_gadget_sve_generated_entries)",
            "NAME(uml_arm64_gadget_sve_generated_entries):"]
    out.extend(f"\t.quad {label}" for label in names)
    out += ["\t.globl NAME(uml_arm64_gadget_sve_generated_entries_end)",
            "NAME(uml_arm64_gadget_sve_generated_entries_end):",
            ".popsection", ""]
    return "\n".join(out)


def segments_text(slot: Slot) -> str:
    parts = []
    for segment in slot.segments:
        if segment.field:
            parts.append(f"detail::sme_field({segment.low}, {segment.width})")
        else:
            parts.append(f"detail::sme_constant({segment.width}, "
                         f"{segment.constant})")
    return ", ".join(parts)


def render_header(forms: list, labels: list, table) -> str:
    slots: list[Slot] = []
    form_lines = []
    row_forms = ["no_form"] * len(table.rows)
    for number, form in enumerate(forms):
        mask = 0
        for bit in form.variant_bits:
            mask |= 1 << bit
        begin = len(slots)
        slots.extend(form.slots)
        for index in form.rows:
            row_forms[index] = str(number)
        form_lines.append(
            f"\tForm{{0x{mask:08x}U, {form.first_entry}U, {begin}U, "
            f"{len(form.slots)}U, Lowering::{form.lowering}, "
            f"{form.argument}U, {form.flags}U, {len(form.variants)}U}}, "
            f"// {form.name}")
    out = ["// SPDX-License-Identifier: GPL-2.0-or-later",
           "// Generated by darwin/cpu/arm64/gadgets/engine/"
           "generate_sve.py. Do not edit.",
           "#pragma once", "",
           "#include <array>", "#include <cstdint>", "",
           '#include "cpu/arm64/detail/sme_encoding_types.hpp"', "",
           "namespace uml::cpu::arm64::gadgets::sve {", "",
           "enum class Lowering : std::uint8_t {"]
    out.extend(f"\t{name}," for name in LOWERINGS)
    out += ["};", "",
            "/* The form's first slot is a general destination, and the",
            " * instruction with XZR there changes nothing. */",
            f"inline constexpr std::uint8_t discard_on_zero = "
            f"{FLAG_DISCARD_ON_ZERO}U;", "",
            "/* One form: its variants are entries [first_entry,",
            " * first_entry + variants) of",
            " * uml_arm64_gadget_sve_generated_entries, selected by the",
            " * word's variant_mask bits in order (a fused WHILE* adds 2 plus",
            " * twice its condition), and its stream holds the CpuState",
            " * offsets of slots[slot_begin, + slot_count) in that order. A",
            " * form without variants selects base entries. */",
            "struct Form final {",
            "\tstd::uint32_t variant_mask;",
            "\tstd::uint32_t first_entry;",
            "\tstd::uint16_t slot_begin;",
            "\tstd::uint8_t slot_count;",
            "\tLowering lowering;",
            "\tstd::uint8_t argument;",
            "\tstd::uint8_t flags;",
            "\tstd::uint16_t variants;",
            "};", "",
            f"inline constexpr std::uint32_t entry_count = {len(labels)};",
            "inline constexpr std::uint16_t no_form = 0xffff;",
            "/* The conditions a WHILE* fuses with, in variant order. */",
            "inline constexpr std::array<std::uint8_t, "
            f"{len(FUSED_CONDITIONS)}> fused_conditions{{{{" +
            ", ".join(f"0x{['eq', 'ne', 'cs', 'cc', 'mi', 'pl'].index(c):x}U"
                      for c, _, _ in FUSED_CONDITIONS) + "}};", ""]
    out.append(f"inline constexpr std::array<detail::SmeOperand, "
               f"{max(1, len(slots))}> slots{{{{")
    for slot in slots:
        out.append(f"\tdetail::SmeOperand{{detail::SmeOperandKind::"
                   f"{slot.kind}, 1, 1, {len(slot.segments)}, "
                   f"{{{{{segments_text(slot)}}}}}}},")
    out += ["}};", "",
            f"inline constexpr std::array<Form, {len(forms)}> forms{{{{"]
    out.extend(form_lines)
    out += ["}};", "",
            f"inline constexpr std::array<std::uint16_t, {len(row_forms)}> "
            "row_forms{{"]
    for start in range(0, len(row_forms), 8):
        out.append("\t" + ", ".join(row_forms[start:start + 8]) + ",")
    out += ["}};", "", "} // namespace uml::cpu::arm64::gadgets::sve", ""]
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--loader", type=Path, required=True)
    parser.add_argument("--table", type=Path, required=True)
    parser.add_argument("--assembly", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    arguments = parser.parse_args()
    table = load_table(arguments.loader, arguments.table)
    forms = build_forms(table)
    labels = assign_entries(forms)
    for path, text in ((arguments.assembly, render_assembly(labels)),
                       (arguments.header,
                        render_header(forms, labels, table))):
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists() or path.read_text() != text:
            path.write_text(text)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except GenerationError as error:
        print(f"generate_sve.py: {error}", file=sys.stderr)
        sys.exit(1)
