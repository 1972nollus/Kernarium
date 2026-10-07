#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Derive the corpus cases of the SVE floating-point forms with FPCR trap
enables set.

The AdvSIMD trapped-exception records (arm64-floating-trap-scalar, -vector
and -control) come from producer unit tests whose check_trap() derives each
expectation from the architecture and the emulator's trap contract
(darwin/cpu/arm64/detail/floating_exception.hpp): operands whose raised
exceptions are known, and an outcome that follows from them. No hardware
observes an SVE trap at PSTATE.SM = 0 either: arm64-builder records the native
SVE cases with every enable clear, and the M4 runs SVE forms only in
streaming mode, where FPProcessException() traps only with FEAT_SME_FA64,
which the SME profile lacks. This script applies the same derivation to
the non-streaming SVE floating-point forms, at VL 16 and in both page
profiles, without an SVL:

- derived: every completed one-instruction case of an SVE floating-point
  form in the arm64-sve-native records, whose exceptions arm64-builder
  observed with every trap disabled. Each runs again with trap enables
  set and FPSR starting with QC: with every enable (FPSR starting with
  IDC as well, unless the form raised it, so that a set flag whose
  enable is set traps nothing), with only the first raised exception's
  enable when the form raised several enabled ones, and with every
  enable but those of the raised exceptions. An enabled raised exception
  gives the floating-point trap: the instruction does not retire, PC and
  every register keep their values, FPSR gains the raised exceptions that
  are not enabled, and the syndrome is EC 0x2c, IL, TFV, and the raised
  enabled exceptions. Otherwise the instruction completes exactly as
  observed.
  A trap leaves the SVE state live, as the SVE access trap Linux takes
  first makes it.
- authored: cases that no observation covers, with operands chosen so
  that IEEE 754 arithmetic decides every raised exception: divide by zero
  alone at each element size; input denormal alone under FZ in single
  and double precision, and nothing under FZ16 in half precision; every
  element's exceptions together; the flags a trap commits; a divide by
  zero, a NaN compare, and an infinite sum in an inactive element, which
  complete; and traps that keep the P destination of a compare and the V
  destination of FADDV and FADDA.

FPRoundBase() (ISA_A64_xml_A_profile-2026-06) never traps the underflow
of a flush to zero, and with FPCR.UFE set it makes an exact tiny result
underflow. The emulator's trap contract decides from the exceptions an
execution with every trap disabled raises, as the AdvSIMD records do, so
neither rule is in these records: a case sets UFE only when it raised
underflow with FZ and FZ16 clear.

    sve-trap-corpus.py sve.jsonl sve-trap.jsonl

The input is a dump that holds the arm64-sve-native records; the output
is it with these records after them, replacing any it held.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import dataclasses
import hashlib
import importlib.util
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent


def _load(name: str, path: pathlib.Path):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


native = _load("sve_native_corpus", HERE / "sve-native-corpus.py")
corpus = native.corpus
sme_native = native.sme_native
sme_table = native.sme_table

SITE = "tools/arm64-tests/sve-trap-corpus.py"
PRODUCER = "arm64-sve-trap"
GROUPS = ("derived", "authored")
VL = native.VL

IOC, DZC, OFC, UFC, IXC, IDC = 0x01, 0x02, 0x04, 0x08, 0x10, 0x80
FLAGS = 0x9F
QC = 1 << 27
ENABLE_SHIFT = 8
ENABLES = FLAGS << ENABLE_SHIFT
UFE = UFC << ENABLE_SHIFT
FZ16 = 1 << 19
FZ = 1 << 24
SYNDROME = 0xB280_0000      # EC 0x2c, IL, TFV
FLOATING = re.compile(r"(f|bf|scvtf|ucvtf)")


def floating(row) -> bool:
    """Whether a row is a floating-point form: the F-, BF-, SCVTF and
    UCVTF encodings, of which the SVE profile has no memory form."""
    return native.executes(row) and FLOATING.match(row.encoding) is not None


def number(fields: dict, name: str, default: int = 0) -> int:
    return int(fields[name], 16) if name in fields else default


def words_of(program: dict) -> tuple[int, ...]:
    return tuple(int(word, 16) for _offset, word in program["words"])


# --------------------------------------------------------------------------
# Derived cases


def allowed_enables(fpcr: int, raised: int) -> int:
    """The enables a case may set: UFE only where FPRoundBase() and the
    trap contract agree, when the form raised underflow without a flush."""
    if fpcr & (FZ | FZ16) or not raised & UFC:
        return ENABLES & ~UFE
    return ENABLES


def eligible(program: dict, case: dict) -> bool:
    words = words_of(program)
    if program["entry"] != 0 or len(words) != 2 or words[1] != native.SVC:
        return False
    if case["event"] != "system-call" or "validator" in case or \
            "memory" in case:
        return False
    initial, expected = case["initial"], case["expected"]
    fpcr, fpsr = number(initial, "fpcr"), number(initial, "fpsr")
    final = number(expected, "fpsr", fpsr)
    return floating(native.row_index().row_for(words[0])) and \
        not fpcr & ENABLES and not fpsr & FLAGS and \
        final & ~FLAGS == fpsr & ~FLAGS


def variant(base: dict, enables: int, fpsr: int, raised: int) -> dict:
    """The base case with these enables and starting FPSR."""
    initial = dict(base["initial"])
    initial["fpcr"] = corpus.hex64(number(initial, "fpcr") | enables)
    initial["fpsr"] = corpus.hex64(fpsr)
    trapped = raised & enables >> ENABLE_SHIFT
    committed = fpsr | raised & ~(enables >> ENABLE_SHIFT)
    item = {key: value for key, value in base.items()
            if key not in ("expected", "initial")}
    item["name"] = f"{base['name']}, enables {enables:#06x}"
    item["initial"] = initial
    if trapped:
        expected = {}
        if committed != fpsr:
            expected["fpsr"] = corpus.hex64(committed)
        if not number(initial, "sve_live"):
            expected["sve_live"] = corpus.hex64(1)
        item.update(event="floating-point-trap", retired=0,
                    syndrome=f"0x{SYNDROME | trapped:08x}")
    else:
        expected = dict(base["expected"])
        expected.pop("fpsr", None)
        if committed != fpsr:
            expected["fpsr"] = corpus.hex64(committed)
    item["expected"] = dict(sorted(expected.items()))
    return item


def derived_cases(program: dict, base: dict) -> list[dict]:
    raised = number(base["expected"], "fpsr") & FLAGS
    allowed = allowed_enables(number(base["initial"], "fpcr"), raised)
    preset = QC | (0 if raised & IDC else IDC)
    cases = [variant(base, allowed, preset, raised)]
    enabled = raised & allowed >> ENABLE_SHIFT
    if enabled & (enabled - 1):
        first = enabled & -enabled
        cases.append(variant(base, first << ENABLE_SHIFT, QC, raised))
    others = allowed & ~(raised << ENABLE_SHIFT)
    if raised and others != allowed:
        cases.append(variant(base, others, QC, raised))
    return cases


def derived(records: list[dict]) -> list[tuple[tuple[int, ...], dict]]:
    """The derived cases of the native records, in their order."""
    items = []
    for record in records:
        if record.get("producer") != native.PRODUCER or \
                tuple(record["profiles"]) != native.BOTH:
            continue
        for case in record["cases"]:
            program = record["programs"][case["program"]]
            if eligible(program, case):
                items += [(words_of(program), item)
                          for item in derived_cases(program, case)]
    return items


# --------------------------------------------------------------------------
# Authored cases

SIZE_FIELD = {2: 1, 4: 2, 8: 3}
ONE = {2: 0x3C00, 4: 0x3F80_0000, 8: 0x3FF0_0000_0000_0000}
DENORMAL = {2: 0x0001, 4: 0x0000_0001, 8: 0x0000_0000_0000_0001}
TWO, THREE, FOUR, SIX = 0x4000_0000, 0x4040_0000, 0x4080_0000, 0x40C0_0000
ONE_AND_HALF = 0x3FC0_0000
MAX, MIN_NORMAL = 0x7F7F_FFFF, 0x0080_0000
INF, NEG_INF, QNAN = 0x7F80_0000, 0xFF80_0000, 0x7FC0_0000
ALL = b"\xff\xff"
ALL_ENABLES = ENABLES
NO_UFE = ENABLES & ~UFE


def vector(esize: int, values: list[int], fill: int | None = None) -> bytes:
    count = VL // esize
    values = values + [values[-1] if fill is None else fill] * \
        (count - len(values))
    return b"".join(value.to_bytes(esize, "little") for value in values)


def inactive(esize: int, element: int) -> bytes:
    """All true but for one element."""
    mask = 0xFFFF & ~(((1 << esize) - 1) << (element * esize))
    return mask.to_bytes(2, "little")


def checked(word: int, encoding: str) -> int:
    row = native.row_index().row_for(word)
    if row is None or row.encoding != encoding or not floating(row):
        raise SystemExit(f"sve-trap-corpus: {word:08x} is not {encoding}")
    return word


def fdiv(esize: int) -> int:
    """FDIV Z0, P0/M, Z0, Z1."""
    return checked(0x650D_8000 | SIZE_FIELD[esize] << 22 | 1 << 5,
                   "fdiv_z_p_zz_")


def fmul(esize: int) -> int:
    """FMUL Z0, P0/M, Z0, Z1."""
    return checked(0x6502_8000 | SIZE_FIELD[esize] << 22 | 1 << 5,
                   "fmul_z_p_zz_")


FCMGE = checked(0x6500_4000 | 2 << 22 | 1 << 16 | 1, "fcmge_p_p_zz_")
FADDV = checked(0x6500_2000 | 2 << 22 | 1 << 5, "faddv_v_p_z_")
FADDA = checked(0x6518_2000 | 2 << 22 | 1 << 5, "fadda_v_p_z_")


@dataclasses.dataclass
class Authored:
    name: str
    word: int
    fpcr: int
    v: dict[int, bytes]
    p: dict[int, bytes]
    raised: int                       # what the operands raise
    result: dict[int, bytes] = dataclasses.field(default_factory=dict)
    predicate: dict[int, bytes] = dataclasses.field(default_factory=dict)


def authored_cases() -> list[Authored]:
    """Each case's raised exceptions follow from IEEE 754 arithmetic on its
    operands: x/0 for finite nonzero x is divide by zero alone, 0/0 and
    inf - inf are invalid operation alone, 1/3 is inexact alone, the
    largest single value divided by the smallest normal one overflows
    (overflow and inexact), x/y for the other quotients here is exact, a
    flushed input is input denormal alone with a product of zero, which is
    exact, FCMGE signals invalid operation for a quiet NaN, and an FP16
    input that FZ16 flushes raises nothing."""
    cases = []
    for esize, label in ((2, "H"), (4, "S"), (8, "D")):
        one = ONE[esize]
        cases.append(Authored(
            f"FDIV {label} 1/0 traps on divide by zero", fdiv(esize),
            DZC << ENABLE_SHIFT,
            {0: vector(esize, [one]), 1: vector(esize, [0, one])},
            {0: ALL}, DZC))
    for esize, label in ((4, "S"), (8, "D")):
        one = ONE[esize]
        cases.append(Authored(
            f"FMUL {label} flushed denormal input traps under FZ",
            fmul(esize), FZ | IDC << ENABLE_SHIFT,
            {0: vector(esize, [DENORMAL[esize], one]),
             1: vector(esize, [one])}, {0: ALL}, IDC))
    cases.append(Authored(
        "FMUL H denormal input flushed by FZ16 raises nothing", fmul(2),
        FZ16 | NO_UFE, {0: vector(2, [DENORMAL[2], ONE[2]]),
                        1: vector(2, [ONE[2]])}, {0: ALL}, 0,
        result={0: vector(2, [0, ONE[2]])}))
    one = ONE[4]
    cases += [
        Authored("FDIV S 1/0 in an inactive element completes", fdiv(4),
                 ALL_ENABLES, {0: vector(4, [one, SIX, SIX, SIX]),
                               1: vector(4, [0, TWO, THREE, FOUR])},
                 {0: inactive(4, 0)}, 0,
                 result={0: vector(4, [one, THREE, TWO, ONE_AND_HALF])}),
        Authored("FDIV S commits inexact with a divide-by-zero trap",
                 fdiv(4), DZC << ENABLE_SHIFT,
                 {0: vector(4, [one]), 1: vector(4, [0, THREE, one])},
                 {0: ALL}, DZC | IXC),
        Authored("FDIV S commits divide by zero with an inexact trap",
                 fdiv(4), IXC << ENABLE_SHIFT,
                 {0: vector(4, [one]), 1: vector(4, [0, THREE, one])},
                 {0: ALL}, DZC | IXC),
        Authored("FDIV S traps on every element's exceptions", fdiv(4),
                 ALL_ENABLES, {0: vector(4, [one, 0, one, MAX]),
                               1: vector(4, [0, 0, THREE, MIN_NORMAL])},
                 {0: ALL}, IOC | DZC | OFC | IXC),
        Authored("FCMGE S NaN traps and keeps its P destination", FCMGE,
                 IOC << ENABLE_SHIFT,
                 {0: vector(4, [QNAN, one, TWO, THREE]),
                  1: vector(4, [one])}, {0: ALL}, IOC),
        Authored("FCMGE S NaN in an inactive element completes", FCMGE,
                 ALL_ENABLES, {0: vector(4, [QNAN, one, TWO, THREE]),
                               1: vector(4, [one])},
                 {0: inactive(4, 0)}, 0,
                 predicate={1: bytes([0x10, 0x11])}),
        Authored("FADDV S inf - inf traps and keeps its V destination",
                 FADDV, IOC << ENABLE_SHIFT,
                 {1: vector(4, [INF, NEG_INF, 0])}, {0: ALL}, IOC),
        Authored("FADDV S -inf in an inactive element completes", FADDV,
                 ALL_ENABLES, {1: vector(4, [INF, NEG_INF, 0])},
                 {0: inactive(4, 1)}, 0,
                 result={0: vector(4, [INF], 0)}),
        Authored("FADDA S inf - inf traps and keeps its V destination",
                 FADDA, IOC << ENABLE_SHIFT,
                 {0: vector(4, [INF, one]),
                  1: vector(4, [NEG_INF, one])}, {0: ALL}, IOC),
        Authored("FADDA S -inf in an inactive element completes", FADDA,
                 ALL_ENABLES, {0: vector(4, [INF, one]),
                               1: vector(4, [NEG_INF, one])},
                 {0: inactive(4, 0)}, 0,
                 result={0: vector(4, [INF], 0)}),
    ]
    return cases


def authored_item(case: Authored) -> tuple[tuple[int, ...], dict]:
    draft = native.Draft(group="authored", name=case.name,
                         words=(case.word,), fpcr=case.fpcr, fpsr=QC,
                         v=dict(case.v), p=dict(case.p))
    wire = draft.wire()
    initial = native.BASE.apply(wire.state)
    item = native.case_skeleton(draft, wire, 0)
    trapped = case.raised & case.fpcr >> ENABLE_SHIFT
    if trapped:
        committed = QC | case.raised & ~(case.fpcr >> ENABLE_SHIFT)
        final = dataclasses.replace(initial, fpsr=committed)
        item.update(event="floating-point-trap", retired=0,
                    expected=native.state_fields(initial, final),
                    syndrome=f"0x{SYNDROME | trapped:08x}")
    else:
        v, p = list(initial.v), list(initial.p)
        for index, value in case.result.items():
            v[index] = value
        for index, value in case.predicate.items():
            p[index] = value
        final = dataclasses.replace(initial, v=v, p=p,
                                    fpsr=QC | case.raised)
        expected = native.state_fields(initial, final)
        expected["pc"] = corpus.hex64(corpus.CODE_BASE + 8)
        item.update(event="system-call", retired=2,
                    expected=dict(sorted(expected.items())))
    return wire.words, item


# --------------------------------------------------------------------------
# Records


def assemble(groups: dict[str, list]) -> list[dict]:
    records = []
    for group in GROUPS:
        items = groups.get(group, [])
        if not items:
            continue
        record = {"record": f"{PRODUCER}/{group}", "producer": PRODUCER,
                  "site": SITE, "profiles": list(native.BOTH),
                  "options": [], "vl": VL, "programs": [], "cases": []}
        index: dict[tuple[int, ...], int] = {}
        for words, item in items:
            if words not in index:
                index[words] = len(record["programs"])
                record["programs"].append({
                    "entry": 0,
                    "words": [[4 * i, f"{word:08x}"]
                              for i, word in enumerate(words)]})
            record["cases"].append(dict(item, program=index[words]))
        records.append(record)
    return records


def records_from(native_records: list[dict]) -> list[dict]:
    return assemble({"derived": derived(native_records),
                     "authored": [authored_item(case)
                                  for case in authored_cases()]})


def script_hash() -> str:
    return hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()


def meta(records: list[dict]) -> dict:
    events: dict[str, int] = {}
    for record in records:
        for case in record["cases"]:
            events[case["event"]] = events.get(case["event"], 0) + 1
    return {
        "script": SITE, "sha256": script_hash(),
        "table": {"file": sme_table.TABLE_PATH,
                  "sha256": native.table().sha256},
        "vl": VL,
        "cases": {record["record"].split("/")[1]: len(record["cases"])
                  for record in records},
        "events": dict(sorted(events.items())),
    }


def strip(lines: list[str]) -> list[str]:
    return [line for line in lines
            if json.loads(line).get("producer") != PRODUCER]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("input", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    options = parser.parse_args()
    header, lines = sme_native.read_dump(options.input)
    lines = strip(lines)
    sources = [item for item in map(json.loads, lines)
               if item.get("producer") == native.PRODUCER]
    if not sources:
        raise SystemExit("sve-trap-corpus: the input holds no native SVE "
                         "records")
    records = records_from(sources)
    header["meta"]["sve_trap"] = meta(records)
    sme_native.write_dump(options.output, header, lines, {}, records)
    print("appended " + ", ".join(
        f"{record['record']} {len(record['cases'])}" for record in records) +
        "; events " + ", ".join(f"{name} {count}" for name, count in
                                header["meta"]["sve_trap"]["events"].items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
