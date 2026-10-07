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

usage: generate_sve.py --loader LOADER --table TABLE
                       --assembly OUT.inc --header OUT.hpp
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import sys
from dataclasses import dataclass, field as dataclass_field
from pathlib import Path

# Element sizes in bytes, by the size field's value.
ESIZES = (1, 2, 4, 8)
ARRANGEMENTS = {1: "16b", 2: "8h", 4: "4s", 8: "2d"}
SIZE_BITS = [22, 23]
SF_BIT = 12

# How the builder fills a form's stream after its slot offsets
# (program_builder_sve.cpp). Base lowerings select base entries.
LOWERINGS = ["plain", "predicate_constant", "predicate_constant_flags",
             "while_compare", "while_conflict", "vector_count",
             "integer_immediate", "bitmask_immediate", "float_immediate",
             "index_table", "count_move", "count_add",
             "vector_length_add"]

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


# Form construction.

def sized(form: Form, template) -> None:
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
        if "sve" not in row.non_streaming_requirement or \
                row.memory is not None:
            continue
        name = row.encoding
        base = name.split("_")[0]
        if name == "pfalse_p_":
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
        names.append(label)
        out += ["\t.p2align 4", f"{label}:"]
        out += [line if line.endswith(":") else "\t" + line
                for line in form.variants[index]]
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
