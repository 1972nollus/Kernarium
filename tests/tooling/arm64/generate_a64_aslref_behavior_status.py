#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate an honest per-allocation A64 behavioral-oracle status table."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import hashlib
import sys
from collections import Counter
from pathlib import Path

from a64_architecture_oracle import SOURCE_ORACLE
from a64_aslref_state_digest import EMPTY_MEMORY_DIGEST, add_specimen_digests
from a64_one_shot_publication import OneShotPublication
from generate_a64_instruction_required_variants import (
    COLUMNS as REQUIRED_COLUMNS,
)
from run_a64_aslref_specimen import ASLREF_VERSION, EVIDENCE_COLUMNS


COLUMNS = [
    "allocation_status_id",
    "stable_id",
    "section",
    "architectural_form",
    "architecture_file",
    "architecture_encoding",
    "decode_class",
    "decode_encoding",
    "allocation_kind",
    "official_semantics_source",
    "aslref_specimen_encoding",
    "aslref_specimen_status",
    "independent_expected_state_digest",
    "terminal_suffix_expected_state_digest",
    "hand_expected_state_status",
    "same_harness_engine_crosscheck",
    "independent_native_hardware_oracle",
    "allocation_behavior_complete",
    "stable_id_behavior_complete",
    "unresolved_requirements",
]

NATIVE_COLUMNS = [
    "oracle_case_id",
    "ctest",
    "case",
    "source",
    "line",
    "encoding",
    "guest_page_size",
    "required_features",
    "observed_features",
    "eligibility",
    "execution_status",
    "terminal_event",
    "observation_boundary",
    "signal",
    "fault_address",
    "initial_state_digest",
    "initial_memory_digest",
    "observed_state_digest",
    "observed_memory_digest",
    "timeout_ms",
    "occurrences",
]

RAW_COLUMNS = [
    "ctest",
    "case",
    "source",
    "line",
    "encoding",
    "invalid",
    "guest_page_size",
    "expected_event",
    "expected_retirement",
    "expected_state_oracle",
    "initial_state_digest",
    "initial_memory_digest",
    "expected_state_digest",
    "expected_memory_digest",
    "expected_fault",
    "expected_syndrome",
    "native_required_features",
    "native_requirement_scope",
    "native_observed_features",
    "native_missing_features",
    "interpreter_result",
    "gadgets_result",
    "jit_result",
    "occurrences",
]

SPECIMEN = {
    "stable_id": "BASE-0005",
    "architecture_file": "add_addsub_imm.xml",
    "architecture_encoding": "ADD_64_addsub_imm",
    "encoding": "910014a0",
    "ctest": "arm64-base-add-sub-immediate",
    "case": "ADD immediate native specimen width=64 sh=0 imm=5 rn=5 rd=0",
    "oracle_case_id": "BASE-0005/ADD_64_addsub_imm",
}


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--required-variants",
        type=Path,
        default=(
            repository
            / "tests/unit/arm64/a64_instruction_required_variants.tsv"
        ),
    )
    parser.add_argument(
        "--engine-evidence",
        type=Path,
        action="append",
        required=True,
        dest="engine_evidence",
    )
    parser.add_argument(
        "--native-evidence",
        type=Path,
        action="append",
        required=True,
        dest="native_evidence",
    )
    parser.add_argument(
        "--specimen-evidence",
        type=Path,
        default=(
            repository / "tests/unit/arm64/a64_aslref_specimen_evidence.tsv"
        ),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=(
            repository / "tests/unit/arm64/a64_aslref_behavior_status.tsv"
        ),
    )
    return parser.parse_args()


def read_tsv(path: Path, columns: list[str]) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != columns:
            raise ValueError(
                f"{path}: columns are {reader.fieldnames!r}, "
                f"expected {columns!r}"
            )
        return list(reader)


def specimen_evidence(path: Path) -> dict[str, str]:
    rows = read_tsv(path, EVIDENCE_COLUMNS)
    if len(rows) != 1:
        raise ValueError(f"{path}: expected one specimen row")
    row = rows[0]
    expected_values = {
        "oracle_case_id": SPECIMEN["oracle_case_id"],
        "stable_id": SPECIMEN["stable_id"],
        "architecture_encoding": SPECIMEN["architecture_encoding"],
        "encoding": SPECIMEN["encoding"],
        "candidate_input_partition": "x5=0000000000000025+nzcv=a0000000",
        "asl_result_x0": "000000000000002a",
        "asl_preserved_x5": "0000000000000025",
        "asl_preserved_nzcv": "a",
        "observation_boundary": "single-instruction-pc+4",
        "memory_digest": EMPTY_MEMORY_DIGEST,
        "strict_typecheck_status": "strict-pass",
        "execution_status": "executed-pass",
        "architecture_oracle": SOURCE_ORACLE,
        "aslref_oracle": ASLREF_VERSION.replace(" ", "-"),
    }
    for column, value in expected_values.items():
        if row[column] != value:
            raise ValueError(
                f"{path}: specimen {column} is {row[column]!r}, expected {value!r}"
            )
    digests = add_specimen_digests(x0=0x2A, x5=0x25, nzcv=0xA)
    for column, key in (
        ("initial_state_digest", "initial"),
        ("independent_expected_state_digest", "instruction"),
        ("terminal_suffix_expected_state_digest", "terminal_suffix"),
    ):
        if row[column] != digests[key]:
            raise ValueError(f"{path}: independently derived {column} drift")
    return row


def engine_profiles(paths: list[Path], specimen: dict[str, str]) -> str:
    profiles: set[str] = set()
    for path in paths:
        matches = [
            row
            for row in read_tsv(path, RAW_COLUMNS)
            if row["ctest"] == SPECIMEN["ctest"]
            and row["case"] == SPECIMEN["case"]
            and row["encoding"] == SPECIMEN["encoding"]
        ]
        if len(matches) != 1:
            raise ValueError(
                f"{path}: expected one exact ASLRef specimen cross-check, "
                f"found {len(matches)}"
            )
        row = matches[0]
        expected = {
            "invalid": "0",
            "expected_event": "system-call",
            "expected_retirement": "exact:2",
            "expected_state_oracle": "exact",
            "native_required_features": "baseline",
            "native_requirement_scope": "instruction",
            "initial_state_digest": specimen["initial_state_digest"],
            "initial_memory_digest": specimen["memory_digest"],
            "expected_state_digest": specimen[
                "terminal_suffix_expected_state_digest"
            ],
            "expected_memory_digest": specimen["memory_digest"],
            "native_missing_features": "none",
            "interpreter_result": "executed-retired",
            "gadgets_result": "executed-retired",
            "jit_result": "executed-retired",
            "occurrences": "1",
        }
        for column, value in expected.items():
            if row[column] != value:
                raise ValueError(
                    f"{path}: specimen {column} is {row[column]!r}, "
                    f"expected {value!r}"
                )
        profiles.add(row["guest_page_size"])
    if not profiles:
        raise ValueError("no same-harness specimen evidence")
    return "+".join(sorted(profiles, key=int))


def native_profiles(paths: list[Path], specimen: dict[str, str]) -> str:
    profiles: set[str] = set()
    for path in paths:
        matches = [
            row for row in read_tsv(path, NATIVE_COLUMNS)
            if row["oracle_case_id"] == SPECIMEN["oracle_case_id"]
            and row["ctest"] == SPECIMEN["ctest"]
            and row["case"] == SPECIMEN["case"]
            and row["encoding"] == SPECIMEN["encoding"]
        ]
        if len(matches) != 1:
            raise ValueError(f"{path}: expected one exact native specimen")
        row = matches[0]
        expected = {
            "required_features": "baseline",
            "eligibility": "eligible",
            "execution_status": "executed",
            "terminal_event": "completed",
            "observation_boundary": specimen["observation_boundary"],
            "signal": "5",
            "fault_address": "0000000000000000",
            "initial_state_digest": specimen["initial_state_digest"],
            "initial_memory_digest": specimen["memory_digest"],
            "observed_state_digest": specimen[
                "independent_expected_state_digest"
            ],
            "observed_memory_digest": specimen["memory_digest"],
            "occurrences": "1",
        }
        for column, value in expected.items():
            if row[column] != value:
                raise ValueError(
                    f"{path}: native {column} is {row[column]!r}, expected {value!r}"
                )
        if row["guest_page_size"] in profiles:
            raise ValueError(f"{path}: duplicate native page profile")
        profiles.add(row["guest_page_size"])
    if profiles != {"4096", "16384"}:
        raise ValueError(
            f"native specimen profiles are {sorted(profiles)!r}, expected 4096/16384"
        )
    return "+".join(sorted(profiles, key=int))


def status_id(row: dict[str, str]) -> str:
    identity = "\0".join(
        row[column]
        for column in (
            "stable_id",
            "architecture_file",
            "architecture_encoding",
            "architecture_mask",
            "architecture_value",
            "architecture_constraints",
            "decode_class",
            "decode_encoding",
            "decode_mask",
            "decode_value",
            "decode_constraints",
        )
    )
    return "behavior-status-" + hashlib.sha256(identity.encode()).hexdigest()[:20]


def main() -> int:
    options = arguments()
    publication = OneShotPublication.prepare(
        [options.output],
        [
            options.required_variants,
            *options.engine_evidence,
            *options.native_evidence,
            options.specimen_evidence,
        ],
    )
    required = read_tsv(options.required_variants, REQUIRED_COLUMNS)
    if len(required) != 2396 or len({row["stable_id"] for row in required}) != 983:
        raise ValueError("required model is not the 2,396-allocation/983-entry model")
    specimen = specimen_evidence(options.specimen_evidence)
    engine_pages = engine_profiles(options.engine_evidence, specimen)
    native_pages = native_profiles(options.native_evidence, specimen)

    output_rows: list[dict[str, str]] = []
    specimen_rows = 0
    for allocation in required:
        is_specimen = all(
            allocation[column] == SPECIMEN[column]
            for column in (
                "stable_id",
                "architecture_file",
                "architecture_encoding",
            )
        )
        if is_specimen:
            specimen_rows += 1
        unresolved = (
            "remaining-semantic-partitions"
            if is_specimen
            else (
                "official-aslref-specimen+"
                "independent-native-hardware-oracle+"
                "semantic-partitions"
            )
        )
        output_rows.append(
            {
                "allocation_status_id": status_id(allocation),
                "stable_id": allocation["stable_id"],
                "section": allocation["section"],
                "architectural_form": allocation["architectural_form"],
                "architecture_file": allocation["architecture_file"],
                "architecture_encoding": allocation["architecture_encoding"],
                "decode_class": allocation["decode_class"],
                "decode_encoding": allocation["decode_encoding"],
                "allocation_kind": allocation["allocation_kind"],
                "official_semantics_source": (
                    f"{allocation['architecture_file']}#"
                    f"{allocation['operation_digest']}"
                ),
                "aslref_specimen_encoding": (
                    SPECIMEN["encoding"] if is_specimen else "none"
                ),
                "aslref_specimen_status": (
                    "strict-executed-independent-state" if is_specimen else "not-run"
                ),
                "independent_expected_state_digest": (
                    specimen["independent_expected_state_digest"]
                    if is_specimen else "none"
                ),
                "terminal_suffix_expected_state_digest": (
                    specimen["terminal_suffix_expected_state_digest"]
                    if is_specimen else "none"
                ),
                "hand_expected_state_status": (
                    "present-not-independent"
                    if is_specimen
                    else "not-reviewed"
                ),
                "same_harness_engine_crosscheck": (
                    f"{engine_pages}:interpreter+gadgets+jit-"
                    "independent-suffix-digest-pass"
                    if is_specimen
                    else "not-joined"
                ),
                "independent_native_hardware_oracle": (
                    f"{native_pages}:eligible+executed+single-instruction-pc+4+"
                    "independent-digest-pass"
                    if is_specimen else "not-run"
                ),
                "allocation_behavior_complete": "0",
                "stable_id_behavior_complete": "0",
                "unresolved_requirements": unresolved,
            }
        )
    if specimen_rows != 1:
        raise ValueError(
            f"required model contains {specimen_rows} exact specimen allocations"
        )
    if any(row["allocation_behavior_complete"] != "0" for row in output_rows):
        raise ValueError("status generator cannot infer behavioral completion")

    output_rows.sort(
        key=lambda row: (row["stable_id"], row["allocation_status_id"])
    )
    publication.publish_tsvs(
        [(publication.targets[0], COLUMNS, output_rows)]
    )
    statuses = Counter(row["aslref_specimen_status"] for row in output_rows)
    print(
        f"A64 ASLRef behavior status: {len(output_rows)} allocations, "
        f"{statuses['strict-executed-independent-state']} independently joined specimen, "
        "0/2396 allocation-complete, 0/983 entry-complete; "
        f"wrote {options.output}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
