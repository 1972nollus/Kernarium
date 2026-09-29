#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Remove unimplemented architecture features from an A64 corpus dump.

usage: absent-feature-corpus.py [--features LIST] [--failing TSV]
                                [--report TSV] DUMP OUTPUT
       absent-feature-corpus.py --collect-failing TSV DUMP LOG...

The virtual CPU implements none of FEAT_MTE, FEAT_GCS, FEAT_CPA, and
FEAT_PAuth: their non-hint instructions are UNDEFINED, their hint-space
instructions are NOPs, and the ID registers report them absent. The producer
checks predate that choice, and their harness enabled these features in most
cases' state, so this script rewrites a dump after export. For each named
feature it

- drops every case that exercises the feature: one that uses its harness
  option, terminal event, or validator relation, or needs it natively without
  expecting the undefined-instruction trap;
- drops every case whose program holds one of the feature's instruction
  encodings, including the hint-space ones whose result the enabled feature
  set, but does not expect it to be undefined;
- strips the feature's state fields, harness option, and allocation tags from
  every other case and record, and drops the cases the --failing file names,
  which fail once that state is gone;
- appends the arm64-absent-<feature> records, which expect every encoding
  class of the feature to be undefined and its hints to be NOPs; and
- replaces the coverage that the --failing cases took from each allocation
  with arm64-absent-mte-tbi cases.

A failing case is one whose tagged address relied on the top-byte ignore that
enabled MTE implied. Each replacement takes a kept two-word case of the same
SIMD&FP load/store allocation that completes through a general-register base
with a zero top byte, sets tbi_enabled, and puts a nonzero tag in the base's
top byte. TBI excludes the top byte from the address and the base is the
instruction's only general register, so the architectural result is the kept
one, except that a written-back base keeps its tag.

Run it on the output of lse2-alignment-corpus.py and --repack the result. To
refresh the failing list, run the rewrite without --failing, run the corpus
test on that pack in both page profiles with --max-failures 0, and pass both
logs to --collect-failing with the rewritten dump.
"""
from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import collections
import csv
import hashlib
import importlib.util
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "build_unit_corpus", HERE / "build-unit-corpus.py")
corpus = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = corpus
_spec.loader.exec_module(corpus)

SITE = "tools/arm64-tests/absent-feature-corpus.py"
DEFAULT_FEATURES = ("mte", "gcs", "cpa", "pauth")
SVC = 0xD400_0001


class Feature:
    def __init__(self, tokens, conditions, fields, validators=(), events=(),
                 options=(), tags=False, encodings=(),
                 stripped_options=()) -> None:
        # Host and allocation feature tokens, and decode-partition conditions.
        self.tokens = frozenset(tokens)
        self.conditions = frozenset(conditions)
        self.fields = frozenset(fields)
        self.validators = frozenset(validators)
        self.events = frozenset(events)
        self.options = frozenset(options)
        # Harness options that nearly every producer check enabled. They are
        # removed from the records rather than dropping them.
        self.stripped_options = frozenset(stripped_options)
        self.tags = tags
        # (mask, value): system encodings that the allocation and partition
        # tables file under a baseline instruction (DC, MRS, MSR).
        self.encodings = tuple(encodings)


FEATURES = {
    "mte": Feature(
        tokens=("mte", "mte2", "mtetc"),
        conditions=("FEAT_MTE", "FEAT_MTE2"),
        fields=("mte_enabled", "mte_exclude", "mte_tag_choices",
                "mte_random_tags", "mte_seed", "mte_tag", "mte_random_state",
                "pstate_tco"),
        validators=("permitted-tag-generator-choice/v1",),
        tags=True,
        encodings=(
            (0xFFFF_F0FF, 0xD503_409F),  # MSR TCO, #imm
            (0xFFDF_FFE0, 0xD51B_42E0),  # MRS and MSR TCO
            *((0xFFFF_FFE0, value) for value in (
                0xD50B_7460, 0xD50B_7480,  # DC GVA, GZVA
                0xD50B_7A60, 0xD50B_7AA0,  # DC CGVAC, CGDVAC
                0xD50B_7C60, 0xD50B_7CA0,  # DC CGVAP, CGDVAP
                0xD50B_7D60, 0xD50B_7DA0,  # DC CGVADP, CGDVADP
                0xD50B_7E60, 0xD50B_7EA0)),  # DC CIGVAC, CIGDVAC
        )),
    "gcs": Feature(
        tokens=("gcs",),
        conditions=("FEAT_GCS",),
        fields=("gcs_pointer", "gcs_implemented", "gcs_push_enabled",
                "gcs_store_enabled"),
        validators=("guarded-control-call-fault-atomicity/v1",
                    "guarded-control-return-fault/v1",
                    "guarded-control-zero-target-fault/v1"),
        events=("gcs-fault",),
        options=("enable_guarded_control_flow",),
        encodings=((0xFFDF_FFE0, 0xD51B_2520),)),  # MRS and MSR GCSPR_EL0
    "cpa": Feature(
        tokens=("cpa",),
        conditions=("FEAT_CPA",),
        fields=("cpa_enabled", "cpa_multiply_enabled")),
    # FEAT_PAuth_LR stays as it was: unimplemented, with its non-hint
    # encodings UNDEFINED and PACM a NOP, so its cases stay valid.
    "pauth": Feature(
        tokens=("pauth",),
        conditions=("FEAT_PAuth",),
        fields=("pointer_auth_keys", "pointer_authentication_enabled"),
        validators=("permitted-transformed-branch-failure/v1",
                    "permitted-transformed-code/v1",
                    "permitted-transformed-generic/v1",
                    "transformed-address-fault-state/v1"),
        stripped_options=("enable_pointer_authentication",),
        # The hint-space PAC*, AUT*, and XPACLRI forms signed, authenticated,
        # or stripped in the producer state.
        encodings=tuple((0xFFFF_FFFF, 0xD503_201F | immediate << 5)
                        for immediate in (7, 8, 10, 12, 14, *range(24, 32)))),
}

# Illegal-word records: one case per encoding class and operand extreme.
# Each word was checked against llvm-mc with the feature enabled (22.1.8 for
# MTE, GCS, and CPA; 23.1.1 for PAuth).
UNDEFINED = {
    "mte": (
        ("ADDG", 0x9181_0420), ("ADDG SP", 0x91BF_3FFF),
        ("SUBG", 0xD181_0420), ("SUBG SP", 0xD1BF_3FFF),
        ("IRG", 0x9AC2_1020), ("IRG SP XZR", 0x9ADF_13FF),
        ("GMI", 0x9AC2_1420), ("GMI SP XZR", 0x9ADF_17FF),
        ("SUBP", 0x9AC2_0020), ("SUBP SP", 0x9ADF_03FF),
        ("SUBPS", 0xBAC2_0020), ("CMPP", 0xBAC2_003F),
        ("CMPP SP", 0xBADF_03FF),
        ("LDG", 0xD960_1020), ("LDG SP", 0xD97F_F3FF),
        ("STG post", 0xD920_1420), ("STG post SP", 0xD93F_F7FF),
        ("STG offset", 0xD920_1820), ("STG offset SP", 0xD93F_FBFF),
        ("STG pre", 0xD920_1C20), ("STG pre SP", 0xD93F_FFFF),
        ("STZG post", 0xD960_1420), ("STZG post SP", 0xD97F_F7FF),
        ("STZG offset", 0xD960_1820), ("STZG offset SP", 0xD97F_FBFF),
        ("STZG pre", 0xD960_1C20), ("STZG pre SP", 0xD97F_FFFF),
        ("ST2G post", 0xD9A0_1420), ("ST2G post SP", 0xD9BF_F7FF),
        ("ST2G offset", 0xD9A0_1820), ("ST2G offset SP", 0xD9BF_FBFF),
        ("ST2G pre", 0xD9A0_1C20), ("ST2G pre SP", 0xD9BF_FFFF),
        ("STZ2G post", 0xD9E0_1420), ("STZ2G post SP", 0xD9FF_F7FF),
        ("STZ2G offset", 0xD9E0_1820), ("STZ2G offset SP", 0xD9FF_FBFF),
        ("STZ2G pre", 0xD9E0_1C20), ("STZ2G pre SP", 0xD9FF_FFFF),
        ("LDGM", 0xD9E0_0020), ("LDGM SP", 0xD9E0_03FF),
        ("STGM", 0xD9A0_0020), ("STGM SP", 0xD9A0_03FF),
        ("STZGM", 0xD920_0020), ("STZGM SP", 0xD920_03FF),
        ("STGP post", 0x6880_8820), ("STGP post SP", 0x68BF_FFFF),
        ("STGP offset", 0x6900_8820), ("STGP offset SP", 0x693F_FFFF),
        ("STGP pre", 0x6980_8820), ("STGP pre SP", 0x69BF_FFFF),
        ("SETGP", 0x1DC2_0420), ("SETGM", 0x1DC2_4420),
        ("SETGE", 0x1DC2_8420), ("SETGPT", 0x1DC2_1420),
        ("SETGMT", 0x1DC2_5420), ("SETGET", 0x1DC2_9420),
        ("SETGPN", 0x1DC2_2420), ("SETGMN", 0x1DC2_6420),
        ("SETGEN", 0x1DC2_A420), ("SETGPTN", 0x1DC2_3420),
        ("SETGMTN", 0x1DC2_7420), ("SETGETN", 0x1DC2_B420),
        ("SETGP XZR", 0x1DDF_07BE),
        ("MSR TCO #1", 0xD503_419F), ("MSR TCO #0", 0xD503_409F),
        ("MRS TCO", 0xD53B_42E0), ("MSR TCO register", 0xD51B_42E0),
        ("DC GVA", 0xD50B_7460), ("DC GZVA", 0xD50B_7480),
        ("DC CGVAC", 0xD50B_7A60), ("DC CGDVAC", 0xD50B_7AA0),
        ("DC CGVAP", 0xD50B_7C60), ("DC CGDVAP", 0xD50B_7CA0),
        ("DC CGVADP", 0xD50B_7D60), ("DC CGDVADP", 0xD50B_7DA0),
        ("DC CIGVAC", 0xD50B_7E60), ("DC CIGDVAC", 0xD50B_7EA0),
    ),
    "gcs": (
        ("GCSPUSHM", 0xD50B_7700), ("GCSPUSHM XZR", 0xD50B_771F),
        ("GCSPOPM", 0xD52B_7720), ("GCSPOPM XZR", 0xD52B_773F),
        ("GCSSS1", 0xD50B_7740), ("GCSSS1 XZR", 0xD50B_775F),
        ("GCSSS2", 0xD52B_7760), ("GCSSS2 XZR", 0xD52B_777F),
        ("GCSPUSHX", 0xD508_779F), ("GCSPOPX", 0xD508_77DF),
        ("GCSPOPCX", 0xD508_77BF),
        ("GCSSTR", 0xD91F_0C20), ("GCSSTR SP", 0xD91F_0FFF),
        ("GCSSTTR", 0xD91F_1C20), ("GCSSTTR SP", 0xD91F_1FFF),
        ("MRS GCSPR_EL0", 0xD53B_2520), ("MSR GCSPR_EL0", 0xD51B_2520),
    ),
    "cpa": (
        ("ADDPT", 0x9A02_2420), ("ADDPT SP", 0x9A1F_3FFF),
        ("SUBPT", 0xDA02_2420), ("SUBPT SP", 0xDA1F_3FFF),
        ("MADDPT", 0x9B62_0C20), ("MADDPT XZR", 0x9B7F_7FFF),
        ("MSUBPT", 0x9B62_8C20), ("MSUBPT XZR", 0x9B7F_FFFF),
    ),
    "pauth": (
        ("PACIA", 0xDAC1_0020), ("PACIA XZR SP", 0xDAC1_03FF),
        ("PACIB", 0xDAC1_0420), ("PACIB SP", 0xDAC1_07FE),
        ("PACDA", 0xDAC1_0820), ("PACDA XZR SP", 0xDAC1_0BFF),
        ("PACDB", 0xDAC1_0C20), ("PACDB SP", 0xDAC1_0FFE),
        ("AUTIA", 0xDAC1_1020), ("AUTIA XZR SP", 0xDAC1_13FF),
        ("AUTIB", 0xDAC1_1420), ("AUTIB SP", 0xDAC1_17FE),
        ("AUTDA", 0xDAC1_1820), ("AUTDA XZR SP", 0xDAC1_1BFF),
        ("AUTDB", 0xDAC1_1C20), ("AUTDB SP", 0xDAC1_1FFE),
        ("PACIZA", 0xDAC1_23E0), ("PACIZA XZR", 0xDAC1_23FF),
        ("PACIZB", 0xDAC1_27E0), ("PACDZA", 0xDAC1_2BE0),
        ("PACDZB", 0xDAC1_2FE0), ("AUTIZA", 0xDAC1_33E0),
        ("AUTIZA XZR", 0xDAC1_33FF), ("AUTIZB", 0xDAC1_37E0),
        ("AUTDZA", 0xDAC1_3BE0), ("AUTDZB", 0xDAC1_3FE0),
        ("AUTDZB XZR", 0xDAC1_3FFF),
        ("XPACI", 0xDAC1_43E0), ("XPACI XZR", 0xDAC1_43FF),
        ("XPACD", 0xDAC1_47E0), ("XPACD XZR", 0xDAC1_47FF),
        ("PACGA", 0x9AC2_3020), ("PACGA XZR SP", 0x9ADF_33FF),
        ("BRAA", 0xD71F_0801), ("BRAA SP", 0xD71F_0BDF),
        ("BRAB", 0xD71F_0C01), ("BLRAA", 0xD73F_0801),
        ("BLRAA SP", 0xD73F_0BDF), ("BLRAB", 0xD73F_0C01),
        ("BRAAZ", 0xD61F_081F), ("BRAAZ X30", 0xD61F_0BDF),
        ("BRABZ", 0xD61F_0C1F), ("BLRAAZ", 0xD63F_081F),
        ("BLRABZ X30", 0xD63F_0FDF),
        ("RETAA", 0xD65F_0BFF), ("RETAB", 0xD65F_0FFF),
        ("ERETAA", 0xD69F_0BFF), ("ERETAB", 0xD69F_0FFF),
        ("LDRAA", 0xF820_0420), ("LDRAA minimum", 0xF860_0420),
        ("LDRAA XZR SP maximum", 0xF83F_F7FF),
        ("LDRAA pre", 0xF820_1C20), ("LDRAA pre SP minimum", 0xF860_0FE0),
        ("LDRAB", 0xF8A0_0420), ("LDRAB XZR SP maximum", 0xF8BF_F7FF),
        ("LDRAB pre", 0xF8FF_FC20),
    ),
}
HINTS = {
    "gcs": (("GCSB DSYNC", 0xD503_227F), ("CHKFEAT X16", 0xD503_251F)),
    "pauth": (
        ("XPACLRI", 0xD503_20FF), ("PACIA1716", 0xD503_211F),
        ("PACIB1716", 0xD503_215F), ("AUTIA1716", 0xD503_219F),
        ("AUTIB1716", 0xD503_21DF), ("PACIAZ", 0xD503_231F),
        ("PACIASP", 0xD503_233F), ("PACIBZ", 0xD503_235F),
        ("PACIBSP", 0xD503_237F), ("AUTIAZ", 0xD503_239F),
        ("AUTIASP", 0xD503_23BF), ("AUTIBZ", 0xD503_23DF),
        ("AUTIBSP", 0xD503_23FF),
    ),
}


def read_failing(path: pathlib.Path | None) -> dict[tuple, str]:
    """(record, case name, 16 KiB ordinal, 4 KiB ordinal) -> feature."""
    if path is None:
        return {}
    failing = {}
    with path.open(encoding="utf-8") as source:
        rows = (line for line in source if not line.startswith("#"))
        for row in csv.DictReader(rows, delimiter="\t"):
            key = (row["record"], row["case"], int(row["ordinal_16k"]),
                   int(row["ordinal_4k"]))
            failing[key] = row["feature"]
    return failing


def is_hint(value: int) -> bool:
    return (value & 0xFFFF_F01F) == 0xD503_201F


class Rewriter:
    def __init__(self, features: tuple[str, ...]) -> None:
        self.features = {name: FEATURES[name] for name in features}
        self.classifier = corpus.Classifier()
        # Every non-hint encoding the features allocate: the allocation
        # predicates, each encoding of the decode partitions, and the system
        # encodings filed under baseline instructions.
        self.encodings: list[tuple[corpus.Pattern, str]] = []
        with corpus.ALLOCATIONS.open(encoding="utf-8") as source:
            for row in csv.DictReader(source, delimiter="\t"):
                tokens = set(row["architecture_native_feature_alternatives"]
                             .replace("|", "+").split("+"))
                self.add(row, lambda feature: bool(tokens & feature.tokens))
        with corpus.PARTITIONS.open(encoding="utf-8") as source:
            for row in csv.DictReader(source, delimiter="\t"):
                if row["partition_kind"] != "allocated":
                    continue
                conditions = set(row["feature_condition"].split(" && "))
                if "FEAT_SVE" in conditions:
                    continue  # SVE is absent, and with it these encodings.
                self.add(row, lambda feature: bool(conditions &
                                                   feature.conditions))
        for name, feature in self.features.items():
            for mask, value in feature.encodings:
                self.encodings.append((corpus.Pattern(
                    f"{mask:08x}", f"{value:08x}", "none"), name))
        self.stripped_options = frozenset().union(*(
            feature.stripped_options for feature in self.features.values()))

    def add(self, row: dict, owned) -> None:
        if is_hint(int(row["architecture_value"], 16)):
            return
        for name, feature in self.features.items():
            if owned(feature):
                self.encodings.append((corpus.Pattern(
                    row["architecture_mask"], row["architecture_value"],
                    row["architecture_constraints"]), name))
                return

    def encoding_feature(self, word: int) -> str | None:
        """The removed feature a word's encoding belongs to, if any."""
        for pattern, name in self.encodings:
            if pattern.matches(word):
                return name
        return None

    def exercised_feature(self, record: dict, case: dict) -> str | None:
        """The removed feature whose behaviour the case checks, if any. A
        case that expects the undefined-instruction trap does not check it
        through its native requirement: without the feature, every encoding
        stays undefined."""
        undefined = case["event"] == "illegal-instruction"
        for name, feature in self.features.items():
            if (not undefined and set(case["native"]) & feature.tokens) or \
                    set(record["options"]) & feature.options or \
                    case["event"] in feature.events or \
                    case.get("validator") in feature.validators:
                return name
        return None

    def strip(self, case: dict) -> bool:
        """Removes the features' state; returns whether any was present."""
        present = False
        for feature in self.features.values():
            for state in (case["initial"], case["expected"]):
                for name in feature.fields:
                    present |= state.pop(name, None) is not None
            if feature.tags:
                for region in case.get("memory", []):
                    present |= bool(region["tags"] or region["expected_tags"])
                    region["tags"] = []
                    region["expected_tags"] = []
        return present


def undefined_record(feature: str) -> dict:
    programs: list[list] = []
    cases = []
    for name, word in UNDEFINED[feature]:
        programs.append({"entry": 0, "words": [[0, f"{word:08x}"],
                                               [4, f"{SVC:08x}"]]})
        cases.append({
            "name": f"{name} is undefined", "program": len(programs) - 1,
            "event": "illegal-instruction", "ordinals": [0, 0],
            "invalid_encoding": True, "minimization": "full", "native": [],
            "initial": {}, "expected": {}, "retired": 0,
        })
    for name, word in HINTS.get(feature, ()):
        programs.append({"entry": 0, "words": [[0, f"{word:08x}"],
                                               [4, f"{SVC:08x}"]]})
        cases.append({
            "name": f"{name} is a NOP", "program": len(programs) - 1,
            "event": "system-call", "ordinals": [0, 0],
            "invalid_encoding": False, "minimization": "full", "native": [],
            "initial": {},
            "expected": {"pc": corpus.hex64(corpus.CODE_BASE + 8)},
            "retired": 2,
        })
    return {
        "record": f"arm64-absent-{feature}", "producer": f"arm64-absent-{feature}",
        "site": SITE, "profiles": ["16k", "4k"], "options": [],
        "programs": programs, "cases": cases,
    }


REPLACEMENT_PRODUCER = "arm64-absent-mte-tbi"
FAILURE = re.compile(r'^FAIL (\S+) case (\d+) ')


def base_register_fields(word: int) -> tuple[int, int]:
    """(Rn, register fields other than Rn) of a load/store word."""
    return (word >> 5) & 31, (word & 31, (word >> 10) & 31, (word >> 16) & 31)


def tbi_variant(case: dict, word: int, tag: int) -> dict | None:
    """The case with TBI enabled and a tagged base, or None if the rule
    does not apply to it."""
    if case["event"] != "system-call" or "validator" in case or \
            "fault" in case or "tbi_enabled" in case["initial"]:
        return None
    base, others = base_register_fields(word)
    key = f"x{base}"
    if base == 31 or base in others or key not in case["initial"]:
        return None
    address = int(case["initial"][key], 16)
    written = case["expected"].get(key)
    if address >> 56 or (written is not None and int(written, 16) >> 56):
        return None
    variant = json.loads(json.dumps(case))
    variant["initial"][key] = corpus.hex64(address | tag << 56)
    variant["initial"]["tbi_enabled"] = corpus.hex64(1)
    if written is not None:
        variant["expected"][key] = corpus.hex64(int(written, 16) | tag << 56)
    variant["name"] = f"{case['name']} with TBI tag 0x{tag:02x}"
    variant["ordinals"] = [0, 0]
    return variant


def replacements(kept: list[tuple[dict, dict, int]], lost: collections.Counter,
                 classifier) -> list[dict]:
    """Replaces each allocation's lost cases from its kept cases."""
    candidates: dict[str, list[int]] = collections.defaultdict(list)
    for index, (_record, _case, word) in enumerate(kept):
        for stable_id in classifier.classify(word):
            # Only SIMD&FP transfers: the base is their one general register.
            if stable_id in lost and stable_id.startswith("FPSIMD-"):
                candidates[stable_id].append(index)
    chosen: dict[int, None] = {}
    for stable_id in sorted(lost):
        pool = [index for index in candidates[stable_id]
                if tbi_variant(kept[index][1], kept[index][2], 1) is not None]
        count = min(lost[stable_id], len(pool))
        for step in range(count):
            chosen.setdefault(pool[step * len(pool) // count])
    records: dict[tuple, dict] = {}
    for serial, index in enumerate(sorted(chosen)):
        record, case, word = kept[index]
        tag = 1 + serial * 37 % 255
        variant = tbi_variant(case, word, tag)
        profiles = tuple(record["profiles"])
        options = tuple(record["options"])
        item = records.get((profiles, options))
        if item is None:
            name = f"{REPLACEMENT_PRODUCER}/{'+'.join(profiles)}"
            if options:
                name += "/" + "+".join(options)
            item = records[(profiles, options)] = {
                "record": name, "producer": REPLACEMENT_PRODUCER,
                "site": SITE, "profiles": list(profiles),
                "options": list(options), "programs": [], "cases": [],
            }
        program = record["programs"][case["program"]]
        if program not in item["programs"]:
            item["programs"].append(program)
        variant["program"] = item["programs"].index(program)
        item["cases"].append(variant)
    return [records[key] for key in sorted(records)]


def collect_failing(output: pathlib.Path, dump: pathlib.Path,
                    logs: list[pathlib.Path]) -> int:
    """Writes the --failing list from corpus-test logs of a rewritten dump."""
    failing = set()
    for log in logs:
        for line in log.read_text(encoding="utf-8").splitlines():
            match = FAILURE.match(line)
            if match:
                failing.add((match.group(1), int(match.group(2))))
    rows = set()
    with dump.open(encoding="utf-8") as source:
        for line in source:
            item = json.loads(line)
            if "cases" not in item:
                continue
            for index, case in enumerate(item["cases"]):
                if (item["record"], index) in failing:
                    if item["producer"].startswith("arm64-absent-"):
                        corpus.fail(f"{item['record']}: authored case "
                                    f"{case['name']} fails")
                    rows.add((item["record"], case["name"], *case["ordinals"]))
                    failing.discard((item["record"], index))
    if failing:
        corpus.fail(f"logs name cases the dump lacks: {sorted(failing)[0]}")
    with output.open("w", encoding="utf-8") as sink:
        sink.write("# Cases that fail once the absent features' state is "
                   "stripped; see absent-feature-corpus.py.\n")
        sink.write("record\tcase\tordinal_16k\tordinal_4k\tfeature\n")
        for record, name, first, second in sorted(rows):
            sink.write(f"{record}\t{name}\t{first}\t{second}\tmte\n")
    print(f"wrote {len(rows)} failing cases to {output}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dump", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path, nargs="+",
                        help="rewritten dump, or corpus-test logs with "
                        "--collect-failing")
    parser.add_argument("--collect-failing", type=pathlib.Path, metavar="TSV",
                        help="write the failing list from corpus-test logs")
    parser.add_argument("--features", default=",".join(DEFAULT_FEATURES),
                        help="comma-separated features (default: %(default)s)")
    parser.add_argument("--failing", type=pathlib.Path,
                        help="cases that fail once the features are stripped")
    parser.add_argument("--report", type=pathlib.Path,
                        help="write per-allocation drop counts as TSV")
    options = parser.parse_args()
    if options.collect_failing is not None:
        return collect_failing(options.collect_failing, options.dump,
                               options.output)
    if len(options.output) != 1:
        parser.error("the rewrite takes one output dump")
    options.output = options.output[0]
    features = tuple(options.features.split(","))
    if not features or set(features) - set(FEATURES) or \
            len(set(features)) != len(features):
        parser.error(f"--features takes names from {', '.join(FEATURES)}")
    failing = read_failing(options.failing)
    rewriter = Rewriter(features)

    lines = options.dump.read_text(encoding="utf-8").splitlines()
    header = json.loads(lines[0])
    blobs: list[str] = []
    records: list[dict] = []
    dropped = collections.Counter()
    by_entry: dict[str, collections.Counter] = collections.defaultdict(
        collections.Counter)
    kept_by_entry = collections.Counter()
    matched_failing = set()
    stripped = 0
    kept_cases: list[tuple[dict, dict, int]] = []
    lost = collections.Counter()
    for line in lines[1:]:
        if not line.strip():
            continue
        item = json.loads(line)
        if "blob" in item:
            blobs.append(line)
            continue
        if item["producer"].startswith("arm64-absent-"):
            continue
        item["options"] = [name for name in item["options"]
                           if name not in rewriter.stripped_options]
        kept = []
        for case in item["cases"]:
            program = item["programs"][case["program"]]
            words = [int(word, 16) for _offset, word in program["words"]]
            entry = int(dict(program["words"])[program["entry"]], 16)
            key = (item["record"], case["name"], *case["ordinals"])
            reason = None
            feature = rewriter.exercised_feature(item, case)
            if feature is not None:
                reason = "feature-case"
            if reason is None and case["event"] != "illegal-instruction":
                feature = next((owner for owner in map(
                    rewriter.encoding_feature, words) if owner), None)
                if feature is not None:
                    reason = "removed-encoding"
            if reason is None:
                stripped += rewriter.strip(case)
                if key in failing:
                    feature = failing[key]
                    reason = "fails-stripped"
                    matched_failing.add(key)
            if reason is not None:
                dropped[(feature, reason)] += 1
                for stable_id in rewriter.classifier.classify(entry) or ("none",):
                    by_entry[stable_id][reason] += 1
                    if reason == "fails-stripped":
                        lost[stable_id] += 1
                continue
            for stable_id in rewriter.classifier.classify(entry) or ("none",):
                kept_by_entry[stable_id] += 1
            kept.append(case)
            if len(words) == 2 and words[1] == SVC and program["entry"] == 0:
                kept_cases.append((item, case, entry))
        if not kept:
            continue
        used = sorted({case["program"] for case in kept})
        renumber = {old: new for new, old in enumerate(used)}
        item["programs"] = [item["programs"][index] for index in used]
        for case in kept:
            case["program"] = renumber[case["program"]]
        item["cases"] = kept
        records.append(item)
    unmatched = set(failing) - matched_failing
    if unmatched:
        corpus.fail(f"--failing names {len(unmatched)} cases the dump lacks "
                    f"or drops otherwise, e.g. {sorted(unmatched)[0]}")
    added = 0
    for feature in features:
        record = undefined_record(feature)
        records.append(record)
        added += len(record["cases"])
    replaced = collections.Counter()
    for record in replacements(kept_cases, lost, rewriter.classifier):
        records.append(record)
        for case in record["cases"]:
            program = record["programs"][case["program"]]
            word = int(dict(program["words"])[program["entry"]], 16)
            for stable_id in rewriter.classifier.classify(word):
                replaced[stable_id] += 1
    replacement_cases = sum(len(record["cases"]) for record in records
                            if record["producer"] == REPLACEMENT_PRODUCER)
    header["meta"]["absent_features"] = {
        "input_sha256": corpus.sha256(options.dump.read_bytes()),
        "script": SITE,
        "sha256": hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
        "features": list(features),
        **({"failing": {
            "file": options.failing.resolve().relative_to(
                corpus.REPOSITORY).as_posix(),
            "sha256": corpus.sha256(options.failing.read_bytes())}}
           if options.failing is not None else {}),
        "dropped_cases": {
            f"{feature}/{reason}": count
            for (feature, reason), count in sorted(dropped.items())},
        "stripped_cases": stripped,
        "added_cases": added,
        "tbi_replacement_cases": replacement_cases,
    }
    with options.output.open("w", encoding="utf-8") as sink:
        sink.write(json.dumps(header, sort_keys=True) + "\n")
        for line in blobs:
            sink.write(line + "\n")
        for record in records:
            sink.write(json.dumps(record, sort_keys=True) + "\n")
    if options.report is not None:
        reasons = ("feature-case", "removed-encoding", "fails-stripped")
        with options.report.open("w", encoding="utf-8") as sink:
            sink.write("stable_id\t" + "\t".join(reasons) +
                       "\tkept\ttbi_replacements\n")
            for stable_id in sorted(set(by_entry) | set(kept_by_entry)):
                counts = by_entry[stable_id]
                sink.write("\t".join([stable_id, *(str(counts[reason])
                                                   for reason in reasons),
                                      str(kept_by_entry[stable_id]),
                                      str(replaced[stable_id])]) + "\n")
    print(f"dropped {sum(dropped.values())} cases "
          f"({', '.join(f'{f}/{r} {n}' for (f, r), n in sorted(dropped.items()))}), "
          f"stripped {stripped}, added {added} absent-feature cases and "
          f"{replacement_cases} TBI replacements")
    return 0


if __name__ == "__main__":
    sys.exit(main())
