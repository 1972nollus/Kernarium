#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate the shared-engine A64 instruction inventory."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import csv
import re
import sys
from collections import Counter
from pathlib import Path


COLUMNS = [
    "stable_id",
    "section",
    "pdf_label",
    "current_suite",
    "current_variant_model",
    "native_feature_requirements",
    "three_engine_status",
]
SECTION_COUNTS = {"Base": 516, "FP&SIMD": 467}
LEDGER_ROW = re.compile(
    r"^- \[x\] Spec \[x\] Interpreter \[x\] Gadget "
    r"\[(?P<jit>[ x])\] JIT \[x\] Tests - (?P<body>.+)$"
)
THREE_ENGINE_STATUSES = {"pending", "verified"}
SUITE_VARIANT_MODELS = {
    "shared-base-control-system": {
        "condition+target-form+register-alias+architectural-state+reserved",
        "condition+target-form+register-alias+flags+reserved",
    },
    "shared-base-data-processing": {
        "register-width+operand-form+register-alias+flags+reserved",
    },
    "shared-base-memory": {
        "access-size+addressing-mode+writeback+register-alias+fault-ordering+reserved",
        "immediate+monitor-state+register-preservation",
    },
    "shared-floating-scalar": {
        "precision+register-alias+fp-state+rounding+reserved",
    },
    "shared-simd-data-processing": {
        "scalar-vector+element-size+arrangement+register-alias+fp-state+reserved",
    },
    "shared-simd-memory": {
        "structure+element-size+addressing-mode+writeback+register-alias+fault-ordering+reserved",
    },
}
FEATURE_TOKENS = {
    "aes",
    "asmv8p2",
    "ats1a",
    "advanced_simd",
    "bf16",
    "brbe",
    "bti",
    "chk",
    "clrbhb",
    "cmh",
    "cmpbr",
    "cpa",
    "crc32",
    "cssc",
    "d128",
    "dgh",
    "dit",
    "dpb",
    "dpb2",
    "dotprod",
    "e3dse",
    "ebep",
    "ebf16",
    "f16f32dot",
    "f16f32mm",
    "f16mm",
    "f8f16mm",
    "f8f32mm",
    "faminmax",
    "fcma",
    "fhm",
    "floating_point",
    "fp16",
    "fp8",
    "fp8dot2",
    "fp8dot4",
    "fp8fma",
    "fprcvt",
    "frintts",
    "flag_m",
    "flag_m2",
    "gcie",
    "gcs",
    "hbc",
    "hinte",
    "i8mm",
    "ite",
    "jscvt",
    "lor",
    "lrcpc",
    "lrcpc2",
    "lrcpc3",
    "ls64",
    "ls64_accdata",
    "ls64_v",
    "lscp",
    "lse",
    "lse128",
    "lsfe",
    "lsui",
    "lut",
    "mec",
    "mops",
    "mte",
    "mte2",
    "mtetc",
    "nmi",
    "occmo",
    "pan",
    "pan2",
    "pauth",
    "pauth_lr",
    "pcdphint",
    "pmull",
    "prfmslc",
    "pops",
    "ras",
    "rdm",
    "rme",
    "rme_gpc3",
    "rprfm",
    "sb",
    "sha1",
    "sha256",
    "sha3",
    "sha512",
    "sm3",
    "sm4",
    "sme",
    "spe",
    "specres",
    "specres2",
    "spe_exc",
    "ssbs",
    "sysinstr128",
    "sysreg128",
    "the",
    "tlbid",
    "tlbios",
    "tlbirange",
    "tlbiw",
    "trbe_exc",
    "trf",
    "uao",
    "vhe",
    "wfxt",
    "xs",
}


def fail(message: str) -> None:
    raise ValueError(message)


def ledger_rows(progress: Path) -> list[tuple[str, str, bool]]:
    rows: list[tuple[str, str, bool]] = []
    section: str | None = None
    for line in progress.read_text(encoding="utf-8").splitlines():
        if line.startswith("## Base instructions"):
            section = "Base"
        elif line.startswith("## SIMD and Floating-point instructions"):
            section = "FP&SIMD"
        elif section is not None and (match := LEDGER_ROW.fullmatch(line)):
            label_and_description = match.group("body")
            label, separator, _ = label_and_description.partition(": ")
            if not separator:
                fail(f"malformed ledger row: {line}")
            rows.append((section, label, match.group("jit") == "x"))
    return rows


def validate_requirement(value: str, stable_id: str) -> None:
    if value == "baseline":
        return
    expression = value
    if value.startswith("per_variant(") and value.endswith(")"):
        expression = value[len("per_variant(") : -1]
    elif "|" in value:
        fail(f"{stable_id}: alternatives require per_variant(...)")
    terms = expression.split("|")
    if not terms or any(not term for term in terms):
        fail(f"{stable_id}: malformed native feature expression")
    for term in terms:
        if term == "baseline":
            continue
        tokens = term.split("+")
        if any(token not in FEATURE_TOKENS for token in tokens):
            fail(f"{stable_id}: unknown native feature in {term!r}")


def main() -> int:
    repository = Path(__file__).resolve().parents[3]
    inventory = repository / "tests/unit/arm64/a64_instruction_inventory.tsv"
    progress = repository / "docs/A64_INSTRUCTION_PROGRESS.md"
    # The feature representation is a neutral platform contract shared by the
    # Apple probe and CPU engines. Neither side owns the other's types.
    host_features = repository / "darwin/platform/host_capabilities.hpp"

    with inventory.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != COLUMNS:
            fail(f"columns are {reader.fieldnames!r}, expected {COLUMNS!r}")
        rows = list(reader)

    if len(FEATURE_TOKENS) != 107:
        fail(f"feature token catalog has {len(FEATURE_TOKENS)} entries, expected 107")
    feature_source = host_features.read_text(encoding="utf-8")
    enum_match = re.search(
        r"enum class ArmFeature\s*:[^{]+\{(?P<body>.*?)"
        r"\binventory_count\s*,",
        feature_source,
        re.DOTALL,
    )
    if enum_match is None:
        fail("cannot parse ArmFeature inventory members")
    enum_features = {
        match.group(1)
        for match in re.finditer(
            r"^\s*([a-z][a-z0-9_]*)\s*,",
            enum_match.group("body"),
            re.MULTILINE,
        )
    }
    if enum_features != FEATURE_TOKENS:
        fail(
            "ArmFeature inventory differs from feature token catalog: "
            f"missing={sorted(FEATURE_TOKENS - enum_features)!r}, "
            f"extra={sorted(enum_features - FEATURE_TOKENS)!r}"
        )
    if len(rows) != 983:
        fail(f"inventory has {len(rows)} rows, expected 983")
    counts = Counter(row["section"] for row in rows)
    if counts != Counter(SECTION_COUNTS):
        fail(f"section counts are {dict(counts)!r}, expected {SECTION_COUNTS!r}")
    status_counts = Counter(row["three_engine_status"] for row in rows)
    verified_sections = Counter(
        row["section"]
        for row in rows
        if row["three_engine_status"] == "verified"
    )

    ids = [row["stable_id"] for row in rows]
    if len(set(ids)) != len(ids):
        fail("stable IDs are not unique")
    identities = [(row["section"], row["pdf_label"]) for row in rows]
    if len(set(identities)) != len(identities):
        fail("(section, PDF label) identities are not unique")

    expected_ids: list[str] = []
    section_indices = Counter()
    for row in rows:
        section = row["section"]
        section_indices[section] += 1
        prefix = "BASE" if section == "Base" else "FPSIMD"
        expected_ids.append(f"{prefix}-{section_indices[section]:04d}")
        for field in ("pdf_label", "current_suite", "current_variant_model"):
            if not row[field].strip():
                fail(f"{row['stable_id']}: {field} is empty")
        suite = row["current_suite"]
        model = row["current_variant_model"]
        if suite not in SUITE_VARIANT_MODELS:
            fail(f"{row['stable_id']}: unknown current suite {suite!r}")
        if model not in SUITE_VARIANT_MODELS[suite]:
            fail(
                f"{row['stable_id']}: variant model {model!r} is not "
                f"valid for suite {suite!r}"
            )
        if row["three_engine_status"] not in THREE_ENGINE_STATUSES:
            fail(
                f"{row['stable_id']}: unknown three-engine status "
                f"{row['three_engine_status']!r}"
            )
        validate_requirement(
            row["native_feature_requirements"], row["stable_id"]
        )
    if ids != expected_ids:
        fail("stable IDs are not contiguous in section order")

    ledger = ledger_rows(progress)
    expected_ledger = [(section, label) for section, label, _ in ledger]
    if identities != expected_ledger:
        mismatch = next(
            (
                index
                for index, pair in enumerate(zip(identities, expected_ledger))
                if pair[0] != pair[1]
            ),
            min(len(identities), len(expected_ledger)),
        )
        fail(f"inventory diverges from progress ledger at row {mismatch + 1}")
    for row, (_, _, jit_complete) in zip(rows, ledger):
        status_complete = row["three_engine_status"] == "verified"
        if status_complete != jit_complete:
            fail(
                f"{row['stable_id']}: inventory status and ledger JIT box differ"
            )

    print(
        "A64 instruction inventory valid: "
        f"{len(rows)} rows "
        f"({counts['Base']} Base, {counts['FP&SIMD']} FP&SIMD); "
        f"{status_counts['verified']} verified "
        f"({verified_sections['Base']} Base, "
        f"{verified_sections['FP&SIMD']} FP&SIMD), "
        f"{status_counts['pending']} pending"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
