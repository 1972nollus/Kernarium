#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Re-derive the A64 instruction corpus's SVE trap records.

tools/arm64-tests/sve-trap-corpus.py derives the arm64-sve-trap records
from the pack's own arm64-sve-native records and authors the cases no
observation covers. This test checks that the pack holds exactly what the
current script derives from the current native records, and checks every
case against the trap contract independently of the script:

- provenance.json names the script and VL 16, and the records' case
  counts and events;
- the records are the script's, case for case: derived from the native
  records in their order, then the authored cases;
- a trap retires nothing, changes no register and not FPSR, leaves the
  SVE state live, and names in its syndrome (EC 0x2c, IL, TFV) one cause
  whose enable is set: for a derived case, the first by priority (ID, IO,
  DZ, OF, UF, IX) of the enabled exceptions its native case raised;
- a completed case retires its instruction and the SVC, and adds no flag
  whose enable is set but underflow under FZ or FZ16, a flush's, which
  never traps; a derived one ends exactly as its native case, but for
  FPSR;
- no derived case sets FPCR.UFE while FZ or FZ16 is set, since its
  native case cannot show whether a flush or an exact tiny result
  underflowed; the authored cases hold both rules.

build-unit-corpus.py --check already refuses a pack whose recorded script
or table hash is stale.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
import json
import pathlib
import sys

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
GENERATOR = REPOSITORY / "tools" / "arm64-tests" / "sve-trap-corpus.py"


def load_generator():
    spec = importlib.util.spec_from_file_location("sve_trap_corpus",
                                                  GENERATOR)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def contract(trap, case: dict, base: dict | None) -> None:
    name = case["name"]
    initial, expected = case["initial"], case["expected"]
    fpcr = trap.number(initial, "fpcr")
    fpsr = trap.number(initial, "fpsr")
    final = trap.number(expected, "fpsr", fpsr)
    enabled = fpcr >> trap.ENABLE_SHIFT & trap.FLAGS
    if base is not None and fpcr & trap.UFE and \
            fpcr & (trap.FZ | trap.FZ16):
        raise SystemExit(f"{name}: UFE with FZ or FZ16")
    flushes = trap.UFC if fpcr & (trap.FZ | trap.FZ16) else 0
    if final & fpsr != fpsr or (final & ~fpsr) & ~trap.FLAGS or \
            (final & ~fpsr) & enabled & ~flushes:
        raise SystemExit(f"{name}: FPSR {final:#x} from {fpsr:#x} under "
                         f"enables {enabled:#x}")
    if case["event"] == "floating-point-trap":
        syndrome = int(case["syndrome"], 16)
        cause = syndrome & ~trap.SYNDROME
        if syndrome & trap.SYNDROME != trap.SYNDROME or not cause or \
                cause & (cause - 1) or cause & ~enabled:
            raise SystemExit(f"{name}: syndrome {syndrome:#010x} under "
                             f"enables {enabled:#x}")
        if base is not None:
            raised = enabled & trap.number(base["expected"], "fpsr") & \
                trap.FLAGS
            first = trap.IDC if raised & trap.IDC else raised & -raised
            if cause != first:
                raise SystemExit(f"{name}: reports {cause:#x}, not "
                                 f"{first:#x}")
        if case["retired"] != 0 or set(expected) - {"sve_live"} or \
                int(expected.get("sve_live", initial.get("sve_live", "0")),
                    16) != 1:
            raise SystemExit(f"{name}: a trap that changes more than the "
                             "live mark")
    elif case["event"] == "system-call":
        pc = trap.corpus.hex64(trap.corpus.CODE_BASE + 8)
        if case["retired"] != 2 or expected.get("pc") != pc:
            raise SystemExit(f"{name}: does not complete after the SVC")
        if base is not None and \
                {key: value for key, value in expected.items()
                 if key != "fpsr"} != \
                {key: value for key, value in base["expected"].items()
                 if key != "fpsr"}:
            raise SystemExit(f"{name}: does not end as its native case")
    else:
        raise SystemExit(f"{name}: event {case['event']}")
    if base is not None and \
            {key: value for key, value in initial.items()
             if key not in ("fpcr", "fpsr")} != \
            {key: value for key, value in base["initial"].items()
             if key not in ("fpcr", "fpsr")}:
        raise SystemExit(f"{name}: does not start as its native case")


def check(trap, sources: list[dict], records: list[dict], meta: dict) -> str:
    if meta.get("script") != trap.SITE or meta.get("vl") != trap.VL:
        raise SystemExit("the pack's trap records name another script or VL")
    expected = trap.records_from(sources)
    if [record["record"] for record in records] != \
            [record["record"] for record in expected]:
        raise SystemExit("the trap records are not the script's groups")
    if meta.get("cases") != {record["record"].split("/")[1]:
                             len(record["cases"]) for record in records}:
        raise SystemExit("the trap records' provenance counts another "
                         "number of cases")
    if meta != trap.meta(records) | {"sha256": meta["sha256"]}:
        raise SystemExit("the trap records' provenance is not the script's")
    bases = {}
    for record in sources:
        for case in record["cases"]:
            if bases.setdefault(case["name"], case) is not case:
                raise SystemExit(f"{case['name']}: two native cases")
    total = 0
    for record, want in zip(records, expected):
        if json.dumps(record, sort_keys=True) != \
                json.dumps(want, sort_keys=True):
            for got, case in zip(record["cases"], want["cases"]):
                if json.dumps(got, sort_keys=True) != \
                        json.dumps(case, sort_keys=True):
                    raise SystemExit(f"{case['name']}: not the script's "
                                     "case")
            raise SystemExit(f"{record['record']}: not the script's record")
        derived = record["record"].endswith("/derived")
        for case in record["cases"]:
            base = bases[case["name"].rsplit(", enables ", 1)[0]] \
                if derived else None
            contract(trap, case, base)
            total += 1
    return (f"SVE trap records: {total} cases in {len(records)} records "
            f"re-derived from {trap.SITE} and the native SVE records")


def main() -> int:
    trap = load_generator()
    builder = trap.corpus
    pack = pathlib.Path(sys.argv[1])
    corpus, tokens = builder.decode_pack(pack.read_bytes())
    items = [item for item in map(json.loads,
                                  builder.dump_corpus(corpus, tokens))
             if item.get("producer") in (trap.PRODUCER,
                                         trap.native.PRODUCER)]
    sources = [item for item in items
               if item["producer"] == trap.native.PRODUCER]
    records = [item for item in items if item["producer"] == trap.PRODUCER]
    meta = corpus.meta.get("sve_trap")
    if meta is None or not records:
        raise SystemExit("the pack's trap records and their provenance "
                         "disagree")
    print(check(trap, sources, records, meta))
    return 0


if __name__ == "__main__":
    sys.exit(main())
