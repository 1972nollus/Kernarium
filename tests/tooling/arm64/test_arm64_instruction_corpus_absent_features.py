#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the absent-feature rewrite of the A64 instruction corpus.

The checked-in pack is absent-feature-corpus.py applied to a dump of the pack
that preceded the removal of FEAT_MTE, FEAT_GCS, and FEAT_CPA. Rebuild that
rewrite from the earlier pack in Git history and require the same bytes, and
require every kept case to equal its source case except for the stripped
feature state and allocation tags.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import collections
import copy
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
TOOLS = REPOSITORY / "tools" / "arm64-tests"
# Git blob of the corpus pack before the features were removed.
PRE_REMOVAL_PACK = "a62f612c8f4c88841eb211560a2d72ffc78fac95"
SKIP = 77


def load(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    builder = load("build_unit_corpus", TOOLS / "build-unit-corpus.py")
    pack = (builder.CORPUS / builder.PACK_NAME).read_bytes()
    corpus, _tokens = builder.decode_pack(pack)
    step = corpus.meta["absent_features"]
    source = subprocess.run(
        ["git", "-C", str(REPOSITORY), "cat-file", "blob", PRE_REMOVAL_PACK],
        capture_output=True, check=False)
    if source.returncode != 0:
        print("skipped: the pre-removal pack is not in this checkout's history")
        return SKIP
    old, old_tokens = builder.decode_pack(source.stdout)
    dump = "".join(line + "\n" for line in builder.dump_corpus(old, old_tokens))
    if hashlib.sha256(dump.encode()).hexdigest() != step["input_sha256"]:
        print("skipped: the pack was rewritten from another input")
        return SKIP

    # The rewrite of the earlier pack reproduces the checked-in pack.
    with tempfile.TemporaryDirectory() as scratch:
        source_dump = pathlib.Path(scratch) / "source.jsonl"
        output = pathlib.Path(scratch) / "absent.jsonl"
        source_dump.write_text(dump, encoding="utf-8")
        subprocess.run(
            [sys.executable, "-B", str(TOOLS / "absent-feature-corpus.py"),
             "--features", ",".join(step["features"]),
             "--failing", str(REPOSITORY / step["failing"]["file"]),
             str(source_dump), str(output)],
            check=True, stdout=subprocess.DEVNULL)
        with output.open(encoding="utf-8") as lines:
            rewritten, tokens = builder.load_dump(lines)
    if builder.encode_pack(rewritten, tokens) != pack:
        raise SystemExit("the rewrite of the earlier pack is not the pack")

    # Every kept case is its source case without the removed state. Cases
    # that share a key keep their order within a record.
    sources: dict[tuple, collections.deque] = collections.defaultdict(
        collections.deque)
    for line in dump.splitlines()[1:]:
        item = json.loads(line)
        if "cases" not in item:
            continue
        for case in item["cases"]:
            key = (item["record"], case["name"], *case["ordinals"])
            case["program"] = item["programs"][case["program"]]
            sources[key].append((item, case))
    retired = builder.RETIRED_FIELD_NAMES
    kept = authored = 0
    for line in builder.dump_corpus(corpus, tokens):
        item = json.loads(line)
        if "cases" not in item:
            continue
        if item["producer"].startswith("arm64-absent-"):
            authored += len(item["cases"])
            continue
        for case in item["cases"]:
            key = (item["record"], case["name"], *case["ordinals"])
            if not sources[key]:
                raise SystemExit(f"{key}: kept case has no source case")
            source_record, source_case = sources[key].popleft()
            if {name: source_record[name] for name in
                    ("producer", "site", "profiles", "options")} != \
                    {name: item[name] for name in
                     ("producer", "site", "profiles", "options")}:
                raise SystemExit(f"{key}: record identity changed")
            expected = copy.deepcopy(source_case)
            for state in (expected["initial"], expected["expected"]):
                for name in retired & state.keys():
                    del state[name]
            for region in expected.get("memory", []):
                region["tags"] = []
                region["expected_tags"] = []
            actual = dict(case, program=item["programs"][case["program"]])
            if actual != expected:
                raise SystemExit(f"{key}: kept case differs from its source "
                                 "beyond the removed state")
            kept += 1
    dropped = sum(step["dropped_cases"].values())
    gone = sum(len(cases) for cases in sources.values())
    if gone != dropped:
        raise SystemExit(f"{gone} source cases are gone, but the rewrite "
                         f"accounts for {dropped}")
    if authored != step["added_cases"] + step["tbi_replacement_cases"]:
        raise SystemExit("authored case count does not match the rewrite")
    print(f"absent-feature rewrite: {kept} kept cases differ from their "
          f"sources only by removed state; {dropped} dropped, "
          f"{authored} authored")
    return 0


if __name__ == "__main__":
    sys.exit(main())
