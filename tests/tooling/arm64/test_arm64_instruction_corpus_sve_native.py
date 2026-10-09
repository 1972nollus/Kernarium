#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Re-derive the A64 instruction corpus's native SVE records.

tools/arm64-tests/sve-native-corpus.py authors the arm64-sve-native records
from the SVE/SME encoding table, and an SVE host decides only their
outcomes. This test re-derives everything else from the current script and
table, so a pack can only hold native records whose request is the one the
script writes now:

- provenance.json names the script's request, script, and table hashes, and
  VL 16;
- the records are the script's groups at VL 16 without harness options, in
  both page profiles but for the 4 KiB record of each group that has one;
- each case is the script's case in order: its name, program, native
  requirement, initial state, and data regions;
- each outcome agrees with the table where the table decides legality, each
  invalid-encoding flag with its outcome, and each retired count with its
  terminal PC and event;
- a case whose SVE state is live ends live, and so does one that retired an
  SVE instruction, which makes the state live;
- exactly the completed first-fault and non-fault loads use the relation
  that excludes the elements they leave CONSTRAINED UNPREDICTABLE, and no
  MOVPRFX is followed by anything but the instruction it prefixes.

build-unit-corpus.py --check already refuses a pack whose recorded script or
table hash is stale, and the builder refuses P or FFR in a state that is not
live.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import importlib.util
import json
import pathlib
import sys

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
GENERATOR = REPOSITORY / "tools" / "arm64-tests" / "sve-native-corpus.py"


def load_generator():
    spec = importlib.util.spec_from_file_location("sve_native_corpus",
                                                  GENERATOR)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def check(native, records: list[dict], meta: dict) -> str:
    request = native.request_bytes()
    if meta.get("request_sha256") != hashlib.sha256(request).hexdigest():
        raise SystemExit("the pack's native records answer another request")
    if meta.get("vl") != native.VL or meta.get("script") != native.SITE:
        raise SystemExit("the pack's native records name another VL or "
                         "script")
    drafts = native.drafts()
    by_key: dict[tuple, list] = {}
    for draft in drafts:
        by_key.setdefault(native.record_key(draft), []).append(draft)
    keys = [key for group in native.GROUPS
            for key in ((group, native.BOTH), (group, ("4k",)))
            if key in by_key]
    if [(record["record"].split("/")[1], tuple(record["profiles"]))
            for record in records] != keys:
        raise SystemExit("the native records are not the script's groups")
    total = 0
    for record, key in zip(records, keys):
        if (record["producer"], record["site"], record.get("vl"),
                record.get("svl", 0), record["options"]) != \
                (native.PRODUCER, native.SITE, native.VL, 0, []):
            raise SystemExit(f"{record['record']}: not the script's record")
        expected = by_key[key]
        if len(record["cases"]) != len(expected):
            raise SystemExit(f"{record['record']}: {len(record['cases'])} "
                             f"cases, the script writes {len(expected)}")
        for draft, case in zip(expected, record["cases"]):
            wire = draft.wire()
            program = record["programs"][case["program"]]
            words = tuple(int(word, 16) for _offset, word in program["words"])
            skeleton = native.case_skeleton(draft, wire, case["program"])
            if words != wire.words or program["entry"] != wire.entry:
                raise SystemExit(f"{draft.name}: not the script's program")
            # Where the table does not decide legality, the hardware decides
            # the invalid-encoding flag with the outcome.
            ignored = {"memory"} | (set() if draft.expect else
                                    {"invalid_encoding"})
            got = {key: case.get(key) for key in skeleton
                   if key not in ignored}
            want = {key: value for key, value in skeleton.items()
                    if key not in ignored}
            if got != want:
                raise SystemExit(f"{draft.name}: not the script's case")
            regions = [{key: value for key, value in region.items()
                        if key != "expected"}
                       for region in case.get("memory", [])]
            if regions != [{key: value for key, value in region.items()
                            if key != "expected"}
                           for region in skeleton.get("memory", [])]:
                raise SystemExit(f"{draft.name}: not the script's regions")
            illegal = case["event"] == "illegal-instruction"
            if draft.expect and (draft.expect == "illegal") != illegal:
                raise SystemExit(f"{draft.name}: {case['event']}, but the "
                                 f"table expects it {draft.expect}")
            if case["invalid_encoding"] != illegal:
                raise SystemExit(f"{draft.name}: invalid-encoding flag and "
                                 "outcome disagree")
            pc = int(case["expected"].get(
                "pc", native.hex64(native.corpus.CODE_BASE)), 16)
            # Each instruction before the PC retired: a completed SVC
            # leaves it after the SVC, a trap at the trapping word. A BRK
            # leaves it at the BRK, which retired as well
            # (terminal_does_not_retire() in darwin/cpu/execution_types.hpp).
            before = (pc - native.corpus.CODE_BASE) // 4
            breakpoint = case["event"] == "breakpoint"
            retired = before + breakpoint
            if case["retired"] != retired:
                raise SystemExit(f"{draft.name}: retired count and PC "
                                 "disagree")
            if case["event"] == "system-call" and \
                    words[before - 1] != native.SVC:
                raise SystemExit(f"{draft.name}: completes with the PC "
                                 "not after an SVC")
            if breakpoint and (before >= len(words) or
                               words[before] & 0xFFE0_001F != 0xD420_0000):
                raise SystemExit(f"{draft.name}: a breakpoint with the PC "
                                 "not at a BRK")
            initial, final = case["initial"], case["expected"]
            was_live = int(initial.get("sve_live", "0"), 16)
            live = int(final.get("sve_live", hex(was_live)), 16)
            ran_sve = any(native.executes(native.row_index().row_for(word))
                          for word in words[:retired])
            if (was_live or ran_sve) and not live:
                raise SystemExit(f"{draft.name}: ends with its SVE state "
                                 "not live")
            entry = native.row_index().row_for(words[0])
            nonfault = entry is not None and entry.memory is not None and \
                entry.memory.shape in native.NONFAULT_SHAPES
            validator = case.get("validator")
            if validator not in (None, native.NONFAULT) or \
                    (validator is not None) != \
                    (nonfault and case["event"] == "system-call"):
                raise SystemExit(f"{draft.name}: validator {validator}")
            if any(native.row_index().row_for(word) is not None and
                   native.row_index().row_for(word).encoding.startswith(
                       "movprfx") and following == native.SVC
                   for word, following in zip(words, words[1:])):
                raise SystemExit(f"{draft.name}: a MOVPRFX prefixes SVC")
            total += 1
    return (f"native SVE records: {total} cases in {len(records)} records "
            f"re-derived from {native.SITE} at VL {native.VL}")


def main() -> int:
    native = load_generator()
    builder = native.corpus
    pack = pathlib.Path(sys.argv[1])
    corpus, tokens = builder.decode_pack(pack.read_bytes())
    records = [item for item in map(json.loads,
                                    builder.dump_corpus(corpus, tokens))
               if item.get("producer") == native.PRODUCER]
    meta = corpus.meta.get("sve_native")
    if meta is None and not records:
        raise SystemExit("the pack holds no native SVE records")
    if meta is None or not records:
        raise SystemExit("the pack's native records and their provenance "
                         "disagree")
    print(check(native, records, meta))
    return 0


if __name__ == "__main__":
    sys.exit(main())
