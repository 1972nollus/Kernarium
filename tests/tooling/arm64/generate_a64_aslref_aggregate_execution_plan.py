#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Map every aggregate evidence identity to an honest ASL execution obligation.

The aggregate manifest does not contain concrete register/vector/memory input
values, only their declared digests and source identities.  Consequently an
allocated aggregate row is mapped exactly to its official allocation but is
kept pending until that exact input is materialized.  Unallocated and
profile-illegal rows receive explicit nonexecution reasons.  No equivalence
between distinct aggregate inputs is inferred.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import hashlib
import sys
from collections import Counter, defaultdict
from pathlib import Path

from a64_aggregate_manifest_identity import (
    read_manifest_identity,
    validate_manifest_identity,
)
from aggregate_a64_raw_instruction_evidence import OUTPUT_COLUMNS as AGGREGATE_COLUMNS
from a64_one_shot_publication import OneShotPublication
from generate_a64_aslref_behavior_status import status_id
from generate_a64_instruction_required_variants import COLUMNS as REQUIRED_COLUMNS


DECODE_COLUMNS = [
    "partition_id",
    "decode_layer",
    "partition_kind",
    "decode_class",
    "partition_name",
    "architecture_file",
    "related_stable_ids",
    "architecture_mask",
    "architecture_value",
    "architecture_constraints",
    "feature_condition",
    "architecture_oracle",
]

COLUMNS = [
    "execution_obligation_id",
    "aggregate_id",
    "allocation_status_id",
    "stable_id",
    "architecture_file",
    "architecture_encoding",
    "decode_partition_id",
    "decode_class",
    "allocation_kind",
    "validity_partition",
    "semantic_input_id",
    "semantic_input_materialization",
    "required_asl_partition",
    "execution_eligibility",
    "nonexecution_reason",
    "guest_page_size",
    "expected_event",
    "expected_retirement",
    "expected_state_oracle",
    "expected_fault_partition",
    "native_required_features",
    "native_requirement_scope",
    "declared_digest",
    "observed_digest",
    "mapping_status",
    "allocation_behavior_complete",
    "unresolved_requirements",
    "architecture_oracle",
]


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--aggregate-manifest",
        type=Path,
        default=repository / "tests/unit/arm64/a64_instruction_aggregate_manifest.tsv",
    )
    parser.add_argument(
        "--aggregate-manifest-identity",
        type=Path,
        default=repository
        / "tests/unit/arm64/a64_instruction_aggregate_manifest.identity.json",
    )
    parser.add_argument(
        "--required-variants",
        type=Path,
        default=repository / "tests/unit/arm64/a64_instruction_required_variants.tsv",
    )
    parser.add_argument(
        "--decode-partitions",
        type=Path,
        default=repository / "tests/unit/arm64/a64_instruction_decode_partitions.tsv",
    )
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def read_tsv(path: Path, columns: list[str]) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != columns:
            raise ValueError(f"{path}: unexpected columns {reader.fieldnames!r}")
        rows = list(reader)
    if not rows:
        raise ValueError(f"{path}: contains no rows")
    return rows


def required_key(row: dict[str, str]) -> tuple[str, ...]:
    return tuple(
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
            "decode_architecture_file",
            "decode_mask",
            "decode_value",
            "decode_constraints",
            "allocation_kind",
        )
    )


def aggregate_key(
    row: dict[str, str], decode: dict[str, str]
) -> tuple[str, ...]:
    return (
        row["stable_id"],
        row["architecture_file"],
        row["architecture_encoding"],
        row["architecture_mask"],
        row["architecture_value"],
        row["architecture_constraints"],
        row["decode_class"],
        decode["partition_name"],
        decode["architecture_file"],
        decode["architecture_mask"],
        decode["architecture_value"],
        decode["architecture_constraints"],
        row["allocation_kind"],
    )


def required_partition(row: dict[str, str]) -> str:
    event = row["expected_event"]
    if event == "memory-fault":
        return "exact-memory-fault-input"
    if event == "floating-point-trap":
        return "exact-fp-trap-input"
    if event == "gcs-fault":
        return "exact-gcs-fault-input"
    if event == "pc-alignment":
        return "exact-pc-alignment-input"
    if event == "instruction-budget":
        return "exact-budget-input"
    if event in {"illegal-instruction", "breakpoint"}:
        return f"exact-{event}-input"
    return "exact-completed-input"


def build_plan(
    aggregates: list[dict[str, str]],
    required: list[dict[str, str]],
    decodes: list[dict[str, str]],
) -> list[dict[str, str]]:
    decode_by_id = {row["partition_id"]: row for row in decodes}
    if len(decode_by_id) != len(decodes):
        raise ValueError("decode partition IDs are not unique")
    required_by_key: dict[tuple[str, ...], list[dict[str, str]]] = defaultdict(list)
    for row in required:
        required_by_key[required_key(row)].append(row)

    result: list[dict[str, str]] = []
    for aggregate in aggregates:
        decode = decode_by_id.get(aggregate["decode_partition_id"])
        if decode is None:
            raise ValueError(
                f"{aggregate['aggregate_id']}: unknown decode partition"
            )
        allocated = aggregate["validity_partition"].startswith("allocated-")
        matches = required_by_key.get(aggregate_key(aggregate, decode), [])
        if allocated and len(matches) != 1:
            raise ValueError(
                f"{aggregate['aggregate_id']}: maps to {len(matches)} allocations"
            )
        if not allocated and matches:
            raise ValueError(
                f"{aggregate['aggregate_id']}: unallocated row mapped to allocation"
            )
        allocation_id = status_id(matches[0]) if matches else "none"
        semantic_input_id = "aggregate-input-" + hashlib.sha256(
            (
                aggregate["aggregate_id"]
                + "\0"
                + aggregate["declared_digest"]
            ).encode()
        ).hexdigest()[:20]
        obligation_id = "asl-obligation-" + hashlib.sha256(
            (
                aggregate["aggregate_id"]
                + "\0"
                + allocation_id
                + "\0"
                + semantic_input_id
            ).encode()
        ).hexdigest()[:20]
        validity = aggregate["validity_partition"]
        if validity == "allocated-valid":
            materialization = "required-not-present-in-aggregate-manifest"
            eligibility = "pending-exact-input-materialization"
            reason = "none"
            unresolved = (
                "exact-register-vector-memory-input+"
                "official-asl-execution+spec-expected-state-event-memory-join"
            )
        elif validity == "allocated-profile-illegal":
            materialization = "not-applicable-profile-illegal"
            eligibility = "unexecutable-declared-profile"
            reason = "aggregate-native-feature-profile-illegal"
            unresolved = "none"
        elif validity == "unallocated":
            materialization = "not-applicable-unallocated"
            eligibility = "unexecutable-unallocated-decode"
            reason = "official-decode-partition-unallocated"
            unresolved = "none"
        else:
            raise ValueError(
                f"{aggregate['aggregate_id']}: unknown validity {validity!r}"
            )
        result.append(
            {
                "execution_obligation_id": obligation_id,
                "aggregate_id": aggregate["aggregate_id"],
                "allocation_status_id": allocation_id,
                "stable_id": aggregate["stable_id"],
                "architecture_file": aggregate["architecture_file"],
                "architecture_encoding": aggregate["architecture_encoding"],
                "decode_partition_id": aggregate["decode_partition_id"],
                "decode_class": aggregate["decode_class"],
                "allocation_kind": aggregate["allocation_kind"],
                "validity_partition": validity,
                "semantic_input_id": semantic_input_id,
                "semantic_input_materialization": materialization,
                "required_asl_partition": required_partition(aggregate),
                "execution_eligibility": eligibility,
                "nonexecution_reason": reason,
                "guest_page_size": aggregate["guest_page_size"],
                "expected_event": aggregate["expected_event"],
                "expected_retirement": aggregate["expected_retirement"],
                "expected_state_oracle": aggregate["expected_state_oracle"],
                "expected_fault_partition": aggregate["expected_fault_partition"],
                "native_required_features": aggregate["native_required_features"],
                "native_requirement_scope": aggregate["native_requirement_scope"],
                "declared_digest": aggregate["declared_digest"],
                "observed_digest": aggregate["observed_digest"],
                "mapping_status": (
                    "exact-allocation" if allocated else "exact-nonallocation"
                ),
                "allocation_behavior_complete": "0",
                "unresolved_requirements": unresolved,
                "architecture_oracle": aggregate["architecture_oracle"],
            }
        )
    result.sort(key=lambda row: row["aggregate_id"])
    return result


def validate_plan(
    aggregates: list[dict[str, str]],
    required: list[dict[str, str]],
    decodes: list[dict[str, str]],
    observed: list[dict[str, str]],
) -> None:
    expected = build_plan(aggregates, required, decodes)
    if observed != expected:
        expected_by_id = {row["aggregate_id"]: row for row in expected}
        observed_by_id = {row["aggregate_id"]: row for row in observed}
        missing = sorted(set(expected_by_id) - set(observed_by_id))
        extra = sorted(set(observed_by_id) - set(expected_by_id))
        changed = sorted(
            identity
            for identity in set(expected_by_id) & set(observed_by_id)
            if expected_by_id[identity] != observed_by_id[identity]
        )
        raise ValueError(
            f"aggregate execution plan drift: missing={missing[:3]}, "
            f"extra={extra[:3]}, changed={changed[:3]}"
        )


def main() -> int:
    options = arguments()
    publication = OneShotPublication.prepare(
        [options.output],
        [
            options.aggregate_manifest,
            options.aggregate_manifest_identity,
            options.required_variants,
            options.decode_partitions,
        ],
    )
    aggregates = read_tsv(options.aggregate_manifest, AGGREGATE_COLUMNS)
    validate_manifest_identity(
        aggregates,
        AGGREGATE_COLUMNS,
        read_manifest_identity(options.aggregate_manifest_identity),
    )
    required = read_tsv(options.required_variants, REQUIRED_COLUMNS)
    decodes = read_tsv(options.decode_partitions, DECODE_COLUMNS)
    if len(required) != 2396:
        raise ValueError("required allocation model is not 2,396 rows")
    rows = build_plan(aggregates, required, decodes)
    validate_plan(aggregates, required, decodes, rows)
    publication.publish_tsvs([(publication.targets[0], COLUMNS, rows)])
    statuses = Counter(row["execution_eligibility"] for row in rows)
    print(
        f"A64 ASLRef aggregate execution plan: {len(rows)}/{len(aggregates)} "
        f"identities mapped, {statuses['pending-exact-input-materialization']} "
        "allocated-valid pending exact inputs, "
        f"{statuses['unexecutable-declared-profile']} profile-illegal, "
        f"{statuses['unexecutable-unallocated-decode']} unallocated, "
        f"0 behavior-complete; wrote {options.output}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
