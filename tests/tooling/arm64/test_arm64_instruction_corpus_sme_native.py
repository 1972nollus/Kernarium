#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Re-derive the A64 instruction corpus's native SME records.

tools/arm64-tests/sme-native-corpus.py authors the arm64-sme-native records
from the SVE/SME encoding table, and an SME host decides only their
outcomes. This test re-derives everything else from the current script and
table, so a pack can only hold native records whose request is the one the
script writes now:

- provenance.json names the script's request, script, and table hashes;
- the records are the script's groups, at its SVL, in both page profiles,
  without harness options;
- each case is the script's case in order: its name, program, native
  requirement, initial state, and data regions;
- each outcome agrees with the table where the table decides legality, and
  each retired count with its terminal PC.

build-unit-corpus.py --check already refuses a pack whose recorded script or
table hash is stale.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import importlib.util
import json
import pathlib
import sys

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
GENERATOR = REPOSITORY / "tools" / "arm64-tests" / "sme-native-corpus.py"


def load_generator():
    spec = importlib.util.spec_from_file_location("sme_native_corpus",
                                                  GENERATOR)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def check(native, records: list[dict], meta: dict) -> str:
    request = native.request_bytes()
    if meta.get("request_sha256") != hashlib.sha256(request).hexdigest():
        raise SystemExit("the pack's native records answer another request")
    if meta.get("svl") != native.SVL or meta.get("script") != native.SITE:
        raise SystemExit("the pack's native records name another SVL or "
                         "script")
    drafts = native.drafts()
    by_group: dict[str, list] = {}
    for draft in drafts:
        by_group.setdefault(draft.group, []).append(draft)
    names = [f"{native.PRODUCER}/{group}" for group in native.GROUPS
             if group in by_group]
    if [record["record"] for record in records] != names:
        raise SystemExit("the native records are not the script's groups")
    total = 0
    for record in records:
        group = record["record"].split("/", 1)[1]
        if (record["producer"], record["site"], record.get("svl"),
                record["profiles"], record["options"]) != \
                (native.PRODUCER, native.SITE, native.SVL, ["16k", "4k"], []):
            raise SystemExit(f"{record['record']}: not the script's record")
        expected = by_group[group]
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
            got = {key: case.get(key) for key in skeleton if key != "memory"}
            want = {key: value for key, value in skeleton.items()
                    if key != "memory"}
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
            pc = int(case["expected"].get(
                "pc", native.hex64(native.corpus.CODE_BASE)), 16)
            # Each instruction before the PC retired: a completed SVC
            # leaves it after the SVC, a trap at the trapping word.
            retired = (pc - native.corpus.CODE_BASE) // 4
            if case["retired"] != retired:
                raise SystemExit(f"{draft.name}: retired count and PC "
                                 "disagree")
            if case["event"] == "system-call" and \
                    words[retired - 1] != native.SVC:
                raise SystemExit(f"{draft.name}: completes with the PC "
                                 "not after an SVC")
            # A literal load of the program's own words loads them, SVCs
            # included, not the oracle's sentinel in their place.
            if native.literal_load(words[0]) and \
                    case["event"] == "system-call":
                size = 4 << (words[0] >> 30)
                loaded = bytes.fromhex(case["expected"].get(
                    f"z{words[0] & 31}", ""))[:size]
                if loaded != b"".join(word.to_bytes(4, "little")
                                      for word in words[:size // 4]):
                    raise SystemExit(f"{draft.name}: the literal load "
                                     "does not load its program")
            total += 1
    return (f"native SME records: {total} cases in {len(records)} records "
            f"re-derived from {native.SITE} at SVL {native.SVL}")


def main() -> int:
    native = load_generator()
    builder = native.corpus
    pack = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else \
        builder.CORPUS / builder.PACK_NAME
    corpus, tokens = builder.decode_pack(pack.read_bytes())
    records = [item for item in map(json.loads,
                                    builder.dump_corpus(corpus, tokens))
               if item.get("producer") == native.PRODUCER]
    meta = corpus.meta.get("sme_native")
    if meta is None and not records:
        raise SystemExit("the pack holds no native SME records")
    if meta is None or not records:
        raise SystemExit("the pack's native records and their provenance "
                         "disagree")
    print(check(native, records, meta))
    return 0


if __name__ == "__main__":
    sys.exit(main())
