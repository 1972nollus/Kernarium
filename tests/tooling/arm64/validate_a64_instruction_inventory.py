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
    "engine_status",
]
# Each section numbers its stable IDs under its own prefix.
SECTION_PREFIXES = {"Base": "BASE", "FP&SIMD": "FPSIMD"}
ENGINE_STATUSES = {"absent", "pending", "verified"}
# Features the virtual CPU omits. An absent entry needs one of them in every
# alternative; its corpus cases expect its encodings to be UNDEFINED.
ABSENT_FEATURES = {
    "cpa", "d128", "gcs", "lor", "lrcpc3", "ls64", "ls64_accdata", "ls64_v",
    "lscp", "lse128", "lsfe", "lsui", "mte", "mte2", "mtetc", "pauth", "the",
}
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
def fail(message: str) -> None:
    raise ValueError(message)


def validate_requirement(
    value: str, stable_id: str, features: set[str]
) -> None:
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
        if any(token not in features for token in tokens):
            fail(f"{stable_id}: unknown native feature in {term!r}")


def main() -> int:
    repository = Path(__file__).resolve().parents[3]
    inventory = repository / "tests/unit/arm64/a64_instruction_inventory.tsv"
    # The feature representation is a neutral platform contract shared by the
    # Apple probe and CPU engines. Neither side owns the other's types.
    host_features = repository / "darwin/platform/host_capabilities.hpp"

    with inventory.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != COLUMNS:
            fail(f"columns are {reader.fieldnames!r}, expected {COLUMNS!r}")
        rows = list(reader)

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
    if not enum_features:
        fail("ArmFeature lists no inventory members")
    counts = Counter(row["section"] for row in rows)
    status_counts = Counter(row["engine_status"] for row in rows)
    verified_sections = Counter(
        row["section"]
        for row in rows
        if row["engine_status"] == "verified"
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
        if section not in SECTION_PREFIXES:
            fail(f"{row['stable_id']}: unknown section {section!r}")
        section_indices[section] += 1
        expected_ids.append(
            f"{SECTION_PREFIXES[section]}-{section_indices[section]:04d}"
        )
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
        if row["engine_status"] not in ENGINE_STATUSES:
            fail(
                f"{row['stable_id']}: unknown engine status "
                f"{row['engine_status']!r}"
            )
        validate_requirement(
            row["native_feature_requirements"], row["stable_id"], enum_features
        )
        if row["engine_status"] == "absent":
            requirement = row["native_feature_requirements"]
            if requirement.startswith("per_variant("):
                requirement = requirement[len("per_variant(") : -1]
            if any(
                not set(term.split("+")) & ABSENT_FEATURES
                for term in requirement.split("|")
            ):
                fail(
                    f"{row['stable_id']}: absent entry needs an omitted "
                    "feature in every alternative"
                )
    if ids != expected_ids:
        fail("stable IDs are not contiguous in section order")

    print(
        "A64 instruction inventory valid: "
        f"{len(rows)} rows "
        f"({counts['Base']} Base, {counts['FP&SIMD']} FP&SIMD); "
        f"{status_counts['verified']} verified "
        f"({verified_sections['Base']} Base, "
        f"{verified_sections['FP&SIMD']} FP&SIMD), "
        f"{status_counts['absent']} absent, "
        f"{status_counts['pending']} pending"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
