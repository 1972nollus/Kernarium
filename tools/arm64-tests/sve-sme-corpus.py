#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Author the corpus cases of the SME forms that SVE executes outside
streaming mode.

On a CPU with FEAT_SVE and FEAT_SME, 29 encodings (33 rows of the generated
SVE/SME encoding table) that FEAT_SME or FEAT_SME2 share with FEAT_SVE2p1
execute at PSTATE.SM = 0: their decode takes FEAT_SME (or FEAT_SME2) in
place of FEAT_SVE2p1, and their execute pseudocode calls only
CheckSVEEnabled(). The table gives them the non-streaming requirement sve
plus sme (plus sme2 for 25). No host observes them there: arm64-builder's
Neoverse V2 has no SME, and the M4 has no SVE. This script records their
outcomes at VL 16, the virtual CPU's one vector length outside streaming
mode, from two sources, chosen per family:

- asl: PSEL, REVD, SCLAMP, UCLAMP, the 2-way SDOT and UDOT (vectors and
  indexed), the six two-register narrowing forms (SQCVTN, SQCVTUN, UQCVTN,
  SQRSHRN, SQRSHRUN, UQRSHRN), and the eight predicate-pair WHILEs, 22
  encodings. Their execute pseudocode (ISA_A64_xml_A_profile-2026-06) is
  integer arithmetic over elements, predicates and general registers whose
  only dependence on the vector length is the element count, `VL DIV
  esize`, so this script transcribes each into a model with the vector
  length as a parameter. A model must reproduce, bit for bit, the whole
  final state of every M4 case of its encoding in streaming mode at VL 64
  that the input holds (arm64-sme-native): its destination, every other
  register, NZCV, FPSR, FPCR, SVCR and ZA. Only then does the script
  evaluate it at VL 16. The models carry the
  parts that are not lane-local: PSEL takes its element modulo the
  element count, and the decrementing WHILEs fill their pair from the
  highest element down.
- m4: FCLAMP, BFMLSLB and BFMLSLT (vectors and indexed), and the
  half-precision FDOT (vectors and indexed), 7 encodings. Their results
  and FPSR flags come from FPMaxNum, FPMinNum, FPMulAdd and FPDotAdd,
  which the M4 decides better than a transcription would. Each is
  lane-local within a 128-bit segment, so a case runs on the M4 in
  streaming mode at VL 64 with every Z and P register holding its VL 16
  value in each segment: every segment computes the VL 16 result and
  raises the VL 16 flags, so the response's low segment and its FPSR are
  the VL 16 outcome. The script requires the segments of every result to
  agree. The same request runs the asl families' cases too, and their
  responses must equal the models at VL 64.

The cases are sve-native-corpus.py's classify and operand cases for these
rows (each witness from a state that is not live, register and field
boundaries, predicate patterns, general sources, and FPCR controls). The
table decides each word's legality; every value of a free field of these
rows decodes, so the models need no hardware for it, and a word no row
decodes goes in a third record, table. The records hold for both page
profiles at VL 16, without an SVL: arm64-sve-sme/asl, /table, and /m4.
Each case names its row's non-streaming requirement, so the runner runs it
only for an engine at sve_support complete whose virtual CPU implements SME
at any SVL (and SME2 for the 25), and skips it everywhere else.

    sve-sme-corpus.py sve.jsonl sve-sme.jsonl --carry PACK
    sve-sme-corpus.py --m4-request m4.request
    sme_native_oracle m4.request m4.response.1          (twice, on the M4)
    sve-sme-corpus.py sve.jsonl sve-sme.jsonl \\
        --m4-response m4.response.1 --m4-response m4.response.2

The input is the dump after the native SVE step; it must hold the native
SME records the models are checked against. --carry PACK copies the m4
record of a pack whose M4 request is unchanged, or none if it has none.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import dataclasses
import functools
import hashlib
import importlib.util
import json
import pathlib
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

SITE = "tools/arm64-tests/sve-sme-corpus.py"
PRODUCER = "arm64-sve-sme"
VL = native.VL
SVL = sme_native.SVL
SEGMENTS = SVL // VL
SM = sme_native.SM
MASK64 = (1 << 64) - 1


@functools.cache
def rows() -> tuple:
    """The rows the SVE profile executes at PSTATE.SM = 0 only with SME."""
    return tuple(row for row in native.table().rows if shared(row))


def shared(row) -> bool:
    if row is None:
        return False
    outside = row.non_streaming_requirement
    return "sve" in outside and "sme" in outside


# --------------------------------------------------------------------------
# Models, transcribed from the execute pseudocode


@dataclasses.dataclass
class Machine:
    vl: int
    x: list[int]
    z: list[bytes]
    p: list[bytes]
    nzcv: int = 0


def get(data: bytes, element: int, bits: int) -> int:
    size = bits // 8
    return int.from_bytes(data[element * size:(element + 1) * size], "little")


def put(data: bytearray, element: int, bits: int, value: int) -> None:
    size = bits // 8
    data[element * size:(element + 1) * size] = \
        (value & ((1 << bits) - 1)).to_bytes(size, "little")


def sint(value: int, bits: int) -> int:
    return value - (1 << bits) if value >> (bits - 1) & 1 else value


def active(predicate: bytes, element: int, esize: int) -> bool:
    bit = element * esize // 8
    return bool(predicate[bit // 8] >> bit % 8 & 1)


def xreg(machine: Machine, number: int) -> int:
    """X{}(n): register 31 is XZR."""
    return 0 if number == 31 else machine.x[number]


def clamp(machine: Machine, word: int, signed: bool) -> None:
    esize = 8 << (word >> 22 & 3)
    n, m, d = word >> 5 & 31, word >> 16 & 31, word & 31
    result = bytearray(machine.vl)
    for e in range(machine.vl * 8 // esize):
        values = [get(machine.z[r], e, esize) for r in (n, m, d)]
        if signed:
            values = [sint(value, esize) for value in values]
        low, high, value = values[2], values[1], values[0]
        put(result, e, esize, min(max(value, low), high))
    machine.z[d] = bytes(result)


def revd(machine: Machine, word: int) -> None:
    g, n, d = word >> 10 & 7, word >> 5 & 31, word & 31
    result = bytearray(machine.z[d])
    for e in range(machine.vl // 16):
        if active(machine.p[g], e, 128):
            segment = machine.z[n][16 * e:16 * e + 16]
            result[16 * e:16 * e + 16] = segment[8:] + segment[:8]
    machine.z[d] = bytes(result)


def dot(machine: Machine, word: int, signed: bool, indexed: bool) -> None:
    n, da = word >> 5 & 31, word & 31
    m = word >> 16 & (7 if indexed else 31)
    index = word >> 19 & 3
    result = bytearray(machine.vl)
    for e in range(machine.vl // 4):
        s = e - e % 4 + index if indexed else e
        total = get(machine.z[da], e, 32)
        for i in range(2):
            a = get(machine.z[n], 2 * e + i, 16)
            b = get(machine.z[m], 2 * s + i, 16)
            if signed:
                a, b = sint(a, 16), sint(b, 16)
            total += a * b
        put(result, e, 32, total)
    machine.z[da] = bytes(result)


def narrow(machine: Machine, word: int, signed_in: bool, signed_out: bool,
           shifted: bool) -> None:
    n, d = (word >> 6 & 15) * 2, word & 31
    shift = 16 - (word >> 16 & 15)
    low, high = (-(1 << 15), (1 << 15) - 1) if signed_out else (0, 0xFFFF)
    result = bytearray(machine.vl)
    for e in range(machine.vl // 4):
        for i in range(2):
            value = get(machine.z[n + i], e, 32)
            if signed_in:
                value = sint(value, 32)
            if shifted:
                value = (value + (1 << (shift - 1))) >> shift
            put(result, 2 * e + i, 16, min(max(value, low), high))
    machine.z[d] = bytes(result)


def psel(machine: Machine, word: int) -> None:
    i1, tszh, tszl = word >> 23 & 1, word >> 22 & 1, word >> 18 & 7
    imm5 = i1 << 4 | tszh << 3 | tszl
    tsz = tszh << 3 | tszl
    if tsz & 1:
        esize, imm = 8, imm5 >> 1
    elif tsz & 2:
        esize, imm = 16, imm5 >> 2
    elif tsz & 4:
        esize, imm = 32, imm5 >> 3
    elif tsz == 8:
        esize, imm = 64, imm5 >> 4
    else:
        raise ValueError(f"{word:08x}: PSEL with tsz 0000 is UNDEFINED")
    n, m, d = word >> 10 & 15, word >> 5 & 15, word & 15
    v = 12 + (word >> 16 & 3)
    elements = machine.vl * 8 // esize
    element = ((machine.x[v] & 0xFFFF_FFFF) + imm) % elements
    machine.p[d] = machine.p[n] if active(machine.p[m], element, esize) \
        else bytes(machine.vl // 8)


# name: (unsigned, comparison, step)
WHILES = {
    "whilelo": (True, "lt", 1), "whilels": (True, "le", 1),
    "whilelt": (False, "lt", 1), "whilele": (False, "le", 1),
    "whilehs": (True, "ge", -1), "whilehi": (True, "gt", -1),
    "whilege": (False, "ge", -1), "whilegt": (False, "gt", -1),
}
COMPARE = {"lt": lambda a, b: a < b, "le": lambda a, b: a <= b,
           "ge": lambda a, b: a >= b, "gt": lambda a, b: a > b}


def while_pair(machine: Machine, word: int, unsigned: bool, comparison: str,
               step: int) -> None:
    esize = 8 << (word >> 22 & 3)
    n, m, d0 = word >> 5 & 31, word >> 16 & 31, (word >> 1 & 7) * 2
    elements = machine.vl * 8 // esize
    pl = machine.vl // 8

    def value(raw: int) -> int:
        return raw if unsigned else sint(raw, 64)

    operand1 = xreg(machine, n)
    element2 = value(xreg(machine, m))
    order = range(2 * elements) if step > 0 else \
        range(2 * elements - 1, -1, -1)
    result = 0
    last = True
    for e in order:
        last = last and COMPARE[comparison](value(operand1), element2)
        if last:
            result |= 1 << (e * esize // 8)
        operand1 = (operand1 + step) & MASK64
    # PredTest over the pair, all of whose elements are active.
    first = result & 1
    none = int(result == 0)
    final = result >> ((2 * elements - 1) * esize // 8) & 1
    machine.nzcv = first << 31 | none << 30 | (1 - final) << 29
    data = result.to_bytes(2 * pl, "little")
    machine.p[d0], machine.p[d0 + 1] = data[:pl], data[pl:]


NARROWING = {"sqcvtn": (True, True, False), "sqcvtun": (True, False, False),
             "uqcvtn": (False, False, False),
             "sqrshrn": (True, True, True), "sqrshrun": (True, False, True),
             "uqrshrn": (False, False, True)}


def model_of(row):
    """The model of an asl-family row, or None for an m4 one."""
    name = row.encoding.split("_")[0]
    if name in ("sclamp", "uclamp"):
        return lambda m, w: clamp(m, w, name == "sclamp")
    if name == "revd":
        return revd
    if name == "psel":
        return psel
    if name in ("sdot", "udot") and "_z32_" in row.encoding:
        return lambda m, w: dot(m, w, name == "sdot",
                                row.encoding.endswith("zzzi_"))
    if name in NARROWING and row.encoding.endswith("_mz2_"):
        return lambda m, w: narrow(m, w, *NARROWING[name])
    if name in WHILES and row.encoding.endswith("_pp_rr_"):
        return lambda m, w: while_pair(m, w, *WHILES[name])
    return None


M4_FAMILIES = ("fclamp", "bfmlslb", "bfmlslt", "fdot")


def source_of(row) -> str:
    if model_of(row) is not None:
        return "asl"
    if row.encoding.split("_")[0] in M4_FAMILIES:
        return "m4"
    raise SystemExit(f"{row.encoding}: no source for this form")


def run_model(row, word: int, machine: Machine) -> Machine:
    machine = dataclasses.replace(machine, x=list(machine.x),
                                  z=list(machine.z), p=list(machine.p))
    model_of(row)(machine, word)
    return machine


# --------------------------------------------------------------------------
# Checking the models against the M4's streaming records


def streaming_state(base, fields: dict):
    """A dumped SME state over a base, as sme-native-corpus.py's Dense."""
    x, v, z, p, za = (list(base.x), list(base.v), list(base.z), list(base.p),
                      list(base.za))
    scalars = {name: getattr(base, name)
               for name in ("nzcv", "fpcr", "fpsr", "svcr", "tpidr2")}
    zt0 = base.zt0
    for key, value in fields.items():
        prefix = key.rstrip("0123456789")
        index = int(key[len(prefix):]) if key != prefix else None
        if key == "sp":
            x[31] = int(value, 16)
        elif key in scalars:
            scalars[key] = int(value, 16)
        elif key == "zt0":
            zt0 = bytes.fromhex(value)
        elif key == "pc":
            continue
        elif index is not None and prefix in ("x", "v", "z", "p", "za"):
            target = {"x": x, "v": v, "z": z, "p": p, "za": za}[prefix]
            target[index] = int(value, 16) if prefix == "x" else \
                bytes.fromhex(value)
        else:
            raise SystemExit(f"sve-sme-corpus: unknown state field {key}")
    return dataclasses.replace(base, x=x, v=v, z=z, p=p, za=za, zt0=zt0,
                               **scalars)


def check_models(lines: list[str]) -> dict[str, int]:
    """Runs each asl-family model on every M4 case of its rows in streaming
    mode that the dump holds, at the M4's VL, and requires the M4's whole
    final state: the destination, every other register, NZCV, FPSR, FPCR,
    SVCR, and ZA. Returns the cases checked per encoding."""
    checked = {row.encoding: 0 for row in rows() if source_of(row) == "asl"}
    failures = []
    base = sme_native.Dense.base(SVL)
    for line in lines:
        item = json.loads(line)
        if item.get("producer") != sme_native.PRODUCER:
            continue
        for case in item["cases"]:
            words = item["programs"][case["program"]]["words"]
            word = int(words[0][1], 16)
            row = native.row_index().row_for(word)
            if not shared(row) or source_of(row) != "asl":
                continue
            initial = streaming_state(base, case["initial"])
            if not initial.svcr & SM:
                continue
            final = streaming_state(initial, case["expected"])
            if case["event"] != "system-call" or case["retired"] != 2 or \
                    len(words) != 2:
                failures.append(f"{case['name']}: {case['event']} after "
                                f"{case['retired']} words")
                continue
            got = run_model(row, word, Machine(
                vl=SVL, x=list(initial.x), z=list(initial.z),
                p=list(initial.p), nzcv=initial.nzcv))
            if dataclasses.replace(initial, x=got.x, z=got.z, p=got.p,
                                   nzcv=got.nzcv) != final:
                failures.append(f"{case['name']}: the model differs from "
                                "the M4")
                continue
            checked[row.encoding] += 1
    unchecked = sorted(name for name, count in checked.items() if not count)
    if failures or unchecked:
        for failure in failures:
            print(f"sve-sme-corpus: {failure}", file=sys.stderr)
        raise SystemExit("sve-sme-corpus: the models do not reproduce the "
                         "M4's records" + (f"; no M4 case for {unchecked}"
                                           if unchecked else ""))
    return checked


# --------------------------------------------------------------------------
# Cases


def group_of(draft) -> str:
    """table for a word no row decodes, else its row's source."""
    if draft.expect == "illegal":
        return "table"
    return source_of(native.row_index().row_for(draft.words[0]))


@functools.cache
def drafts() -> tuple:
    """sve-native-corpus.py's classify and operand cases of the rows, in
    that order. The table decides each word: every value of these rows'
    free fields decodes (their decode pseudocode has no UNDEFINED case but
    the feature test, PSEL's tsz 0000 aside, which no row decodes), so a
    word a row decodes is legal."""
    items = native.classify_drafts(rows(), shared) + \
        native.operand_drafts(rows())
    for draft in items:
        if not draft.expect:
            draft.expect = "legal"
        draft.group = group_of(draft)
    return tuple(items)


def initial_of(draft):
    """The case on the wire and its VL 16 initial state. A state that is
    not live has zero P and FFR, which Linux keeps when the first SVE
    instruction makes it live."""
    case = draft.wire()
    return case, native.BASE.apply(case.state)


def result_of(case, initial, final, event: str = "system-call"):
    """The VL 16 oracle's response for a final state."""
    state = native.WireState(
        x={i: final.x[i] for i in range(32) if final.x[i] != initial.x[i]},
        nzcv=final.nzcv, fpcr=final.fpcr, fpsr=final.fpsr,
        v={i: final.v[i] for i in range(32) if final.v[i] != initial.v[i]},
        live=final.live,
        p={i: final.p[i] for i in range(16) if final.p[i] != initial.p[i]},
        ffr=final.ffr if final.ffr != initial.ffr else None)
    completed = event == "system-call"
    return native.WireResult(
        event=event, pc_offset=4 * (len(case.words) - 1) if completed else 0,
        fault_address=0, esr=0,
        sve_size=native.SVE_LIVE_SIZE if final.live else
        native.SVE_HEADER_SIZE, state=state, runs=())


def model_item(draft) -> dict:
    case, initial = initial_of(draft)
    if draft.expect == "illegal":
        return native.record_case(draft, case, result_of(
            case, initial, initial, "illegal-instruction"), 0)
    row = native.row_index().row_for(case.words[0])
    machine = run_model(row, case.words[0], Machine(
        vl=VL, x=list(initial.x), z=list(initial.v), p=list(initial.p),
        nzcv=initial.nzcv))
    final = dataclasses.replace(initial, x=machine.x, v=machine.z,
                                p=machine.p, nzcv=machine.nzcv, live=1)
    return native.record_case(draft, case, result_of(case, initial, final),
                              0)


def replicated(data: bytes) -> bytes:
    return data * SEGMENTS


def m4_case(draft):
    """The streaming case at VL 64 whose every segment is the VL 16
    case."""
    case, initial = initial_of(draft)
    state = sme_native.WireState(
        x={**draft.x, 18: 0}, nzcv=initial.nzcv, fpcr=initial.fpcr,
        fpsr=initial.fpsr, svcr=SM,
        z={i: replicated(initial.v[i]) for i in range(32)},
        p={i: replicated(initial.p[i]) for i in range(16)})
    return sme_native.WireCase(words=case.words, entry=0, state=state)


def m4_drafts() -> tuple:
    """The cases the M4 runs: every legal case of the rows."""
    return tuple(draft for draft in drafts() if draft.group != "table")


def m4_request() -> bytes:
    return sme_native.write_request([m4_case(draft) for draft in
                                     m4_drafts()], SVL)


def m4_final(draft, result):
    """The M4's final Z, P, NZCV and FPSR of a case, which must leave the
    general registers, FPCR and SVCR alone."""
    request = m4_case(draft).state
    if result.event != "system-call" or result.pc_offset != 4:
        raise native.Finding(f"{draft.name}: {result.event} at offset "
                             f"{result.pc_offset} on the M4")
    x = {i: request.x.get(i, corpus.FILL_REGISTERS[i]) for i in range(32)}
    if any(x[i] != value for i, value in result.state.x.items()) or \
            (result.state.fpcr, result.state.svcr) != (request.fpcr, SM):
        raise native.Finding(f"{draft.name}: changed a general register, "
                             "FPCR or SVCR on the M4")
    z = [result.state.z.get(i, request.z[i]) for i in range(32)]
    p = [result.state.p.get(i, request.p[i]) for i in range(16)]
    return x, z, p, result.state.nzcv, result.state.fpsr


def m4_item(draft, result) -> dict:
    """The VL 16 record case of an m4-family case from its response: every
    segment of every register must agree, and the low one is the VL 16
    result."""
    case, initial = initial_of(draft)
    _x, z, p, nzcv, fpsr = m4_final(draft, result)
    for name, registers, size in (("z", z, VL), ("p", p, VL // 8)):
        for i, data in enumerate(registers):
            if data != replicated(data[:size]):
                raise native.Finding(f"{draft.name}: the segments of "
                                     f"{name}{i} differ")
    final = dataclasses.replace(
        initial, v=[data[:VL] for data in z], p=[data[:VL // 8] for data in p],
        nzcv=nzcv, fpsr=fpsr, live=1)
    return native.record_case(draft, case, result_of(case, initial, final),
                              0)


def check_m4(draft, result) -> None:
    """An asl-family case on the M4 must be its model at VL 64."""
    x, z, p, nzcv, fpsr = m4_final(draft, result)
    request = m4_case(draft)
    state = request.state
    row = native.row_index().row_for(request.words[0])
    machine = run_model(row, request.words[0], Machine(
        vl=SVL, x=[x[i] for i in range(32)], z=[state.z[i] for i in
                                                 range(32)],
        p=[state.p[i] for i in range(16)], nzcv=state.nzcv))
    if (z, p, nzcv, fpsr) != (machine.z, machine.p, machine.nzcv,
                              state.fpsr):
        raise native.Finding(f"{draft.name}: the M4 differs from the model")


# --------------------------------------------------------------------------
# Records

GROUPS = ("asl", "table", "m4")


def assemble(items: list) -> list[dict]:
    """The records holding the cases, each given with its draft and its
    program's words, in group order."""
    records: dict[str, dict] = {}
    programs: dict[str, dict] = {}
    for draft, words, item in items:
        record = records.setdefault(draft.group, {
            "record": f"{PRODUCER}/{draft.group}", "producer": PRODUCER,
            "site": SITE, "profiles": list(native.BOTH), "options": [],
            "vl": VL, "programs": [], "cases": []})
        index = programs.setdefault(draft.group, {})
        if words not in index:
            index[words] = len(record["programs"])
            record["programs"].append({
                "entry": 0,
                "words": [[4 * i, f"{word:08x}"]
                          for i, word in enumerate(words)]})
        record["cases"].append(dict(item, program=index[words]))
    return [records[group] for group in GROUPS if group in records]


def derived_items() -> list:
    """The asl and table cases, which need no hardware."""
    return [(draft, draft.wire().words, model_item(draft))
            for draft in drafts() if draft.group != "m4"]


def m4_items(results: list) -> list:
    """The m4 cases from a response, after checking the asl families'
    cases in it against their models."""
    items = []
    findings = []
    for draft, result in zip(m4_drafts(), results):
        try:
            if draft.group == "asl":
                check_m4(draft, result)
            else:
                items.append((draft, draft.wire().words,
                              m4_item(draft, result)))
        except native.Finding as finding:
            findings.append(str(finding))
    if findings:
        for finding in findings:
            print(f"sve-sme-corpus: {finding}", file=sys.stderr)
        raise SystemExit(f"sve-sme-corpus: {len(findings)} M4 cases disagree")
    return items


def script_hash() -> str:
    return hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()


def counts_of(records: list[dict]) -> dict[str, int]:
    return {record["record"].split("/")[1]: len(record["cases"])
            for record in records}


def meta(checked: dict, m4: dict | None, records: list[dict]) -> dict:
    return {
        "script": SITE, "sha256": script_hash(),
        "table": {"file": sme_table.TABLE_PATH,
                  "sha256": native.table().sha256},
        "vl": VL, "cases": counts_of(records),
        "models_checked": dict(sorted(checked.items())),
        "m4_request_sha256": hashlib.sha256(m4_request()).hexdigest(),
        **({"m4": m4} if m4 is not None else {}),
    }


def strip(lines: list[str]) -> list[str]:
    return [line for line in lines
            if json.loads(line).get("producer") != PRODUCER]


def carried_m4(pack_path: pathlib.Path) -> tuple[dict | None, list]:
    """The m4 records of a pack, which must answer this M4 request."""
    pack, tokens = corpus.decode_pack(pack_path.read_bytes())
    step = pack.meta.get("sve_sme", {}).get("m4")
    records = [record for record in pack.records
               if record.producer == PRODUCER and
               record.name == f"{PRODUCER}/m4"]
    if step is None and not records:
        return None, []
    if step is None or step["request_sha256"] != \
            hashlib.sha256(m4_request()).hexdigest():
        raise SystemExit("sve-sme-corpus: the M4 request changed since the "
                         "pack's m4 records; run the M4 again")
    dumped = [json.loads(line) for line in corpus.dump_corpus(
        corpus.Corpus(meta=pack.meta, blobs=pack.blobs, records=records),
        tokens)]
    cases = [(item, case) for item in dumped if "cases" in item
             for case in item["cases"]]
    expected = [draft for draft in drafts() if draft.group == "m4"]
    if len(cases) != len(expected):
        raise SystemExit("sve-sme-corpus: the pack's m4 cases do not answer "
                         "the request")
    pairs = []
    for draft, (item, case) in zip(expected, cases):
        words = tuple(int(word, 16) for _offset, word in
                      item["programs"][case["program"]]["words"])
        if case["name"] != draft.name or words != draft.wire().words:
            raise SystemExit(f"sve-sme-corpus: {draft.name}: the pack's m4 "
                             "cases do not answer the request")
        pairs.append((draft, words, case))
    return step, pairs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("input", type=pathlib.Path, nargs="?")
    parser.add_argument("output", type=pathlib.Path, nargs="?")
    parser.add_argument("--m4-request", type=pathlib.Path,
                        help="write the M4's request and exit")
    parser.add_argument("--m4-response", type=pathlib.Path, action="append",
                        default=[], help="an M4 response to the request; "
                        "several must give identical records")
    parser.add_argument("--carry", type=pathlib.Path,
                        help="copy the m4 records of this pack, if any")
    options = parser.parse_args()
    if options.m4_request is not None:
        request = m4_request()
        options.m4_request.write_bytes(request)
        print(f"wrote {len(m4_drafts())} M4 cases, {len(request)} bytes, "
              f"sha256 {hashlib.sha256(request).hexdigest()}")
        return 0
    if options.input is None or options.output is None or \
            bool(options.m4_response) == (options.carry is not None):
        parser.error("give INPUT OUTPUT and either --m4-response or --carry")
    header, lines = sme_native.read_dump(options.input)
    lines = strip(lines)
    checked = check_models(lines)
    items = derived_items()
    if options.m4_response:
        cases = [m4_case(draft) for draft in m4_drafts()]
        runs = [sme_native.read_response(path.read_bytes(), cases, SVL)
                for path in options.m4_response]
        built = [m4_items(results) for _host, results in runs]
        if any([item for _d, _w, item in other] !=
               [item for _d, _w, item in built[0]] for other in built[1:]):
            raise SystemExit("sve-sme-corpus: the M4 responses give "
                             "different records")
        host = runs[0][0]
        if host["svl"] != SVL or not {"FEAT_SME", "FEAT_SME2"} <= \
                set(host["features"]):
            raise SystemExit("sve-sme-corpus: the response is not from an "
                             f"SME2 host with a {SVL}-byte SVL")
        items += built[0]
        m4 = {"request_sha256": hashlib.sha256(m4_request()).hexdigest(),
              "svl": SVL, "host_features": host["features"],
              "cases": len(cases)}
    else:
        m4, pairs = carried_m4(options.carry)
        items += pairs
    records = assemble(items)
    header["meta"]["sve_sme"] = meta(checked, m4, records)
    sme_native.write_dump(options.output, header, lines, {}, records)
    print(f"appended {sum(len(record['cases']) for record in records)} SVE/"
          f"SME cases: " + ", ".join(f"{name} {count}" for name, count in
                                     counts_of(records).items()) +
          f"; the models reproduce {sum(checked.values())} M4 cases of "
          f"{len(checked)} encodings")
    return 0


if __name__ == "__main__":
    sys.exit(main())
