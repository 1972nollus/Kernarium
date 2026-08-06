#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate exact 4K/16K raw-evidence aggregates against a checked manifest."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import hashlib
import re
import sys
from collections import Counter
from pathlib import Path

from a64_aggregate_manifest_identity import (
    read_manifest_identity,
    validate_manifest_identity,
)
from a64_raw_evidence_files import (
    exclusive_evidence_locks,
    open_raw_text,
    raw_evidence_file,
)
from aggregate_a64_raw_instruction_evidence import (
    EXECUTED,
    OUTPUT_COLUMNS,
    RAW_COLUMNS,
    aggregate_id,
    parse_constraints,
    possible_semantic_feature_sets,
)
from generate_a64_instruction_decode_partitions import COLUMNS as DECODE_COLUMNS
from generate_a64_instruction_required_variants import COLUMNS as REQUIRED_COLUMNS
from generate_a64_instruction_allocations import exact_features


SHA256 = re.compile(r"[0-9a-f]{64}\Z")
SITE = re.compile(r"[0-9a-f]{20}\Z")
CUSTOM = re.compile(r"custom-validator@[A-Za-z0-9_.+-]+\.cpp:[1-9][0-9]*\Z")
COUNTER_ITEM = re.compile(r"(?:\+)?([^:]+?):([1-9][0-9]*)(?=\+|\Z)")
PROFILE_INDEPENDENT = [
    column
    for column in OUTPUT_COLUMNS
    if column not in {
        "aggregate_id",
        "guest_page_size",
        "declared_digest",
    }
]
NATIVE_STATUSES = {
    "exact",
    "semantic-feature-gate",
    "architectural-feature-fallback",
    "overlapping-allocation",
    "program-level-gate",
    "not-applicable-invalid",
    "not-applicable",
}
PROFILE_EXCEPTION_COLUMNS = [
    "ctest",
    "case",
    "source_file",
    "source_line",
    "encoding",
    "guest_page_size",
    "stable_id",
    "architecture_encoding",
    "reason",
]


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument("--actual-4k", type=Path, required=True)
    parser.add_argument("--actual-16k", type=Path, required=True)
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
    parser.add_argument(
        "--manifest",
        type=Path,
        default=repository / "tests/unit/arm64/a64_instruction_aggregate_manifest.tsv",
    )
    parser.add_argument(
        "--manifest-identity",
        type=Path,
        default=repository
        / "tests/unit/arm64/a64_instruction_aggregate_manifest.identity.json",
    )
    parser.add_argument(
        "--profile-exceptions",
        type=Path,
        default=(
            repository
            / "tests/unit/arm64/a64_instruction_profile_exceptions.tsv"
        ),
    )
    parser.add_argument("--evidence-4k", type=Path, required=True)
    parser.add_argument("--evidence-16k", type=Path, required=True)
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


def parse_counter(value: str, field: str) -> Counter[str]:
    result: Counter[str] = Counter()
    if not value:
        raise ValueError(f"{field}: empty result counter")
    position = 0
    while position < len(value):
        match = COUNTER_ITEM.match(value, position)
        if match is None:
            raise ValueError(f"{field}: malformed result counter {value!r}")
        name, count = match.groups()
        if name in result:
            raise ValueError(f"{field}: duplicate result {name!r}")
        result[name] = int(count)
        position = match.end()
    return result


def validate_rows(
    rows: list[dict[str, str]],
    page_size: str,
    required_rows: list[dict[str, str]],
    decode_rows: list[dict[str, str]],
) -> None:
    decode_by_id = {row["partition_id"]: row for row in decode_rows}
    decode_by_identity = {
        (
            row["decode_class"], row["partition_name"],
            row["architecture_file"], row["architecture_mask"],
            row["architecture_value"], row["architecture_constraints"],
        ): row
        for row in decode_rows
        if row["decode_layer"] == "instruction-table"
    }
    required_by_allocation = {
        (
            row["stable_id"], row["architecture_encoding"],
            row["architecture_mask"], row["architecture_value"],
            row["architecture_constraints"], row["decode_class"],
            row["decode_encoding"], row["decode_architecture_file"],
            row["decode_mask"], row["decode_value"],
            row["decode_constraints"],
        ): row
        for row in required_rows
    }
    ids: set[str] = set()
    identities: set[tuple[str, ...]] = set()
    for index, row in enumerate(rows, start=2):
        where = f"aggregate row {index}"
        if row["guest_page_size"] != page_size:
            raise ValueError(f"{where}: wrong guest page size")
        if row["aggregate_id"] != aggregate_id(row):
            raise ValueError(f"{where}: aggregate ID does not match its identity")
        if row["aggregate_id"] in ids:
            raise ValueError(f"{where}: duplicate aggregate ID")
        ids.add(row["aggregate_id"])
        identity = tuple(row[column] for column in OUTPUT_COLUMNS[:26])
        if identity in identities:
            raise ValueError(f"{where}: duplicate aggregate")
        identities.add(identity)
        decode = decode_by_id.get(row["decode_partition_id"])
        if decode is None or decode["decode_class"] != row["decode_class"]:
            raise ValueError(f"{where}: unknown or swapped decode partition")
        allocation_key = (
            row["stable_id"], row["architecture_encoding"],
            row["architecture_mask"], row["architecture_value"],
            row["architecture_constraints"], row["decode_class"],
            decode["partition_name"], decode["architecture_file"],
            decode["architecture_mask"], decode["architecture_value"],
            decode["architecture_constraints"],
        )
        if row["allocation_kind"] in {"primary", "alias"}:
            required = required_by_allocation.get(allocation_key)
            if required is None:
                raise ValueError(f"{where}: unknown or swapped required allocation")
            expected_decode = decode_by_identity.get(
                (
                    required["decode_class"], required["decode_encoding"],
                    required["decode_architecture_file"],
                    required["decode_mask"], required["decode_value"],
                    required["decode_constraints"],
                )
            )
            if expected_decode is None or expected_decode["partition_id"] != row["decode_partition_id"]:
                raise ValueError(f"{where}: required allocation decode drift")
            if exact_features(row["semantic_native_features"]) not in (
                possible_semantic_feature_sets(required)
            ):
                raise ValueError(
                    f"{where}: semantic feature dependency drift"
                )
            exact_features(row["native_fallback_features"])
        else:
            for field in (
                "architecture_file", "architecture_mask", "architecture_value",
                "architecture_constraints",
            ):
                if row[field] != decode[field]:
                    raise ValueError(
                        f"{where}: reserved decode predicate drift in {field}"
                    )
            if row["semantic_native_features"] != "baseline":
                raise ValueError(
                    f"{where}: reserved row has semantic feature dependencies"
                )
            if row["native_fallback_features"] != "baseline":
                raise ValueError(
                    f"{where}: reserved row has native feature fallbacks"
                )
        expected_site = hashlib.sha256(
            f"{row['producer']}:{row['source_file']}:{row['source_line']}".encode()
        ).hexdigest()[:20]
        if SITE.fullmatch(row["source_site"]) is None or row["source_site"] != expected_site:
            raise ValueError(f"{where}: malformed source-site digest")
        if not row["source_line"].isdigit() or int(row["source_line"]) <= 0:
            raise ValueError(f"{where}: malformed source line")
        if row["native_requirement_status"] == "drift":
            raise ValueError(f"{where}: unrelated or missing native feature gate")
        if row["native_requirement_status"] not in NATIVE_STATUSES:
            raise ValueError(f"{where}: invalid native requirement status")
        if row["native_requirement_scope"] not in {"instruction", "program"}:
            raise ValueError(f"{where}: invalid native requirement scope")
        if (
            row["native_requirement_status"] == "program-level-gate"
            and row["native_requirement_scope"] != "program"
        ):
            raise ValueError(
                f"{where}: program-level gate has instruction scope"
            )
        if row["expected_state_oracle"].startswith("custom-validator@") and (
            CUSTOM.fullmatch(row["expected_state_oracle"]) is None
        ):
            raise ValueError(f"{where}: malformed custom-oracle locator")
        declared = int(row["declared_count"])
        unique = int(row["unique_word_count"])
        if declared <= 0 or unique <= 0 or unique > declared:
            raise ValueError(f"{where}: invalid aggregate counts")
        for engine in ("interpreter", "gadgets", "jit"):
            results = parse_counter(row[f"{engine}_results"], where)
            if sum(results.values()) != declared:
                raise ValueError(f"{where}: {engine} result count drift")
            executed = sum(
                count for name, count in results.items() if name in EXECUTED
            )
            if int(row[f"{engine}_executed_count"]) != executed:
                raise ValueError(f"{where}: {engine} executed count drift")
        for field in (
            "native_observed_results", "native_missing_results", "decoder_results"
        ):
            if sum(parse_counter(row[field], where).values()) != declared:
                raise ValueError(f"{where}: {field} count drift")
        for field in ("declared_digest", "observed_digest"):
            if SHA256.fullmatch(row[field]) is None:
                raise ValueError(f"{where}: malformed {field}")

    stable_ids = {
        row["stable_id"]
        for row in rows
        if row["allocation_kind"] in {"primary", "alias"}
    }
    required_ids = {row["stable_id"] for row in required_rows}
    if stable_ids != required_ids:
        missing = sorted(required_ids - stable_ids)
        extra = sorted(stable_ids - required_ids)
        raise ValueError(
            f"{page_size}: stable-ID coverage mismatch; missing={missing[:5]!r}, "
            f"extra={extra[:5]!r}"
        )
    required_allocations = {
        (
            row["stable_id"],
            row["architecture_encoding"],
            row["architecture_mask"],
            row["architecture_value"],
            row["architecture_constraints"],
            row["decode_class"], row["decode_encoding"],
            row["decode_architecture_file"],
            row["decode_mask"], row["decode_value"],
            row["decode_constraints"],
        )
        for row in required_rows
    }
    observed_allocations = {
        (
            row["stable_id"],
            row["architecture_encoding"],
            row["architecture_mask"],
            row["architecture_value"],
            row["architecture_constraints"],
            row["decode_class"],
            decode_by_id[row["decode_partition_id"]]["partition_name"],
            decode_by_id[row["decode_partition_id"]]["architecture_file"],
            decode_by_id[row["decode_partition_id"]]["architecture_mask"],
            decode_by_id[row["decode_partition_id"]]["architecture_value"],
            decode_by_id[row["decode_partition_id"]]["architecture_constraints"],
        )
        for row in rows
        if row["allocation_kind"] in {"primary", "alias"}
    }
    if observed_allocations != required_allocations:
        missing = sorted(required_allocations - observed_allocations)
        extra = sorted(observed_allocations - required_allocations)
        raise ValueError(
            f"{page_size}: required allocation coverage mismatch; "
            f"missing={missing[:3]!r}, extra={extra[:3]!r}"
        )


def compare_profiles(
    rows_4k: list[dict[str, str]],
    rows_16k: list[dict[str, str]],
    excluded_4k: set[str] | None = None,
    excluded_16k: set[str] | None = None,
) -> None:
    excluded_4k = excluded_4k or set()
    excluded_16k = excluded_16k or set()

    def normalized(rows: list[dict[str, str]]) -> Counter[tuple[str, ...]]:
        return Counter(tuple(row[column] for column in PROFILE_INDEPENDENT) for row in rows)

    left = normalized(
        [row for row in rows_4k if row["aggregate_id"] not in excluded_4k]
    )
    right = normalized(
        [row for row in rows_16k if row["aggregate_id"] not in excluded_16k]
    )
    if left != right:
        missing = list((left - right).elements())[:1]
        extra = list((right - left).elements())[:1]
        raise ValueError(
            "4K/16K aggregate semantics differ; "
            f"16K-missing={missing!r}, 16K-extra={extra!r}"
        )


def normalized_source(source: str) -> str:
    marker = "/tests/"
    return "tests/" + source.split(marker, 1)[1] if marker in source else source


def exception_raw_rows(
    directory: Path, exception: dict[str, str]
) -> list[dict[str, str]]:
    entry = raw_evidence_file(directory, exception["ctest"])
    with open_raw_text(entry) as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != RAW_COLUMNS:
            raise ValueError(
                f"{entry.path}: unexpected columns {reader.fieldnames!r}"
            )
        rows = list(reader)
    if not rows:
        raise ValueError(f"{entry.path}: contains no rows")
    return [
        row
        for row in rows
        if row["ctest"] == exception["ctest"]
        and row["case"] == exception["case"]
        and normalized_source(row["source"]) == exception["source_file"]
        and row["line"] == exception["source_line"]
        and row["encoding"] == exception["encoding"]
    ]


def required_word_matches(row: dict[str, str], word: int) -> bool:
    for mask_column, value_column in (
        ("architecture_mask", "architecture_value"),
        ("decode_mask", "decode_value"),
    ):
        mask = int(row[mask_column], 16)
        if word & mask != int(row[value_column], 16):
            return False
    return all(
        (
            operator == "=="
        ) == all(
            expected_bit == "x" or expected_bit == actual_bit
            for expected_bit, actual_bit in zip(
                expected,
                f"{(word >> low) & ((1 << width) - 1):0{width}b}",
            )
        )
        for low, width, operator, expected in parse_constraints(
            row["architecture_constraints"], row["decode_constraints"]
        )
    )


def validate_profile_exceptions(
    exceptions: list[dict[str, str]],
    rows_4k: list[dict[str, str]],
    rows_16k: list[dict[str, str]],
    required_rows: list[dict[str, str]],
    evidence_4k: Path,
    evidence_16k: Path,
) -> tuple[set[str], set[str]]:
    excluded_4k: set[str] = set()
    excluded_16k: set[str] = set()
    identities: set[tuple[str, ...]] = set()
    for index, exception in enumerate(exceptions, start=2):
        where = f"profile exception row {index}"
        identity = tuple(exception[column] for column in PROFILE_EXCEPTION_COLUMNS[:-1])
        if identity in identities:
            raise ValueError(f"{where}: duplicate exception identity")
        identities.add(identity)
        if not exception["reason"].strip():
            raise ValueError(f"{where}: empty reason")
        if exception["guest_page_size"] != "16384":
            raise ValueError(f"{where}: only an exact 16K-only case is supported")
        matches_4k = exception_raw_rows(evidence_4k, exception)
        matches_16k = exception_raw_rows(evidence_16k, exception)
        if matches_4k:
            raise ValueError(f"{where}: declared 16K-only case exists in 4K raw evidence")
        if len(matches_16k) != 1:
            raise ValueError(
                f"{where}: expected one 16K raw row, found {len(matches_16k)}"
            )
        raw = matches_16k[0]
        if raw["guest_page_size"] != "16384" or raw["native_requirement_scope"] != "program":
            raise ValueError(f"{where}: exception is not a 16K program-scope case")
        if raw["invalid"] != "0" or raw["occurrences"] != "1":
            raise ValueError(f"{where}: exception is not one valid occurrence")
        word = int(exception["encoding"], 16)
        required = [
            row
            for row in required_rows
            if row["stable_id"] == exception["stable_id"]
            and row["architecture_encoding"]
            == exception["architecture_encoding"]
            and required_word_matches(row, word)
        ]
        if len(required) != 1:
            raise ValueError(
                f"{where}: exception word maps to {len(required)} required allocations"
            )
        allocation = required[0]
        aggregate_matches = [
            row
            for row in rows_16k
            if row["producer"] == exception["ctest"]
            and row["source_file"] == exception["source_file"]
            and row["source_line"] == exception["source_line"]
            and row["stable_id"] == exception["stable_id"]
            and row["architecture_encoding"]
            == exception["architecture_encoding"]
            and row["native_requirement_scope"] == "program"
            and row["declared_count"] == "1"
            and row["unique_word_count"] == "1"
        ]
        if len(aggregate_matches) != 1:
            raise ValueError(
                f"{where}: maps to {len(aggregate_matches)} 16K aggregates"
            )
        target = aggregate_matches[0]
        excluded_16k.add(target["aggregate_id"])
        allocation_identity = (
            allocation["stable_id"],
            allocation["architecture_encoding"],
            allocation["architecture_mask"],
            allocation["architecture_value"],
            allocation["architecture_constraints"],
        )

        def independently_covered(
            rows: list[dict[str, str]], excluded: set[str]
        ) -> bool:
            return any(
                row["aggregate_id"] not in excluded
                and (
                    row["stable_id"],
                    row["architecture_encoding"],
                    row["architecture_mask"],
                    row["architecture_value"],
                    row["architecture_constraints"],
                )
                == allocation_identity
                for row in rows
            )

        if not independently_covered(rows_4k, excluded_4k):
            raise ValueError(f"{where}: allocation lacks independent 4K coverage")
        if not independently_covered(rows_16k, excluded_16k):
            raise ValueError(f"{where}: allocation lacks other 16K coverage")
    return excluded_4k, excluded_16k


def compare_manifest(
    actual: list[dict[str, str]], manifest: list[dict[str, str]]
) -> None:
    if actual != manifest:
        raise ValueError("aggregate evidence differs from checked manifest")


def read_bound_manifest(
    manifest_path: Path, identity_path: Path
) -> list[dict[str, str]]:
    manifest = read_tsv(manifest_path, OUTPUT_COLUMNS)
    validate_manifest_identity(
        manifest,
        OUTPUT_COLUMNS,
        read_manifest_identity(identity_path),
    )
    return manifest


def canonical_combined_rows(
    rows_4k: list[dict[str, str]], rows_16k: list[dict[str, str]]
) -> list[dict[str, str]]:
    return sorted(rows_4k + rows_16k, key=lambda row: row["aggregate_id"])


def run(options: argparse.Namespace) -> int:
    required = read_tsv(options.required_variants, REQUIRED_COLUMNS)
    decode = read_tsv(options.decode_partitions, DECODE_COLUMNS)
    rows_4k = read_tsv(options.actual_4k, OUTPUT_COLUMNS)
    rows_16k = read_tsv(options.actual_16k, OUTPUT_COLUMNS)
    exceptions = read_tsv(
        options.profile_exceptions, PROFILE_EXCEPTION_COLUMNS
    )
    validate_rows(rows_4k, "4096", required, decode)
    validate_rows(rows_16k, "16384", required, decode)
    excluded_4k, excluded_16k = validate_profile_exceptions(
        exceptions,
        rows_4k,
        rows_16k,
        required,
        options.evidence_4k,
        options.evidence_16k,
    )
    compare_profiles(rows_4k, rows_16k, excluded_4k, excluded_16k)
    combined = canonical_combined_rows(rows_4k, rows_16k)
    manifest = read_bound_manifest(options.manifest, options.manifest_identity)
    compare_manifest(combined, manifest)
    print(
        f"A64 aggregate manifest: 983/983 IDs, {len(rows_4k)} 4K and "
        f"{len(rows_16k)} 16K aggregates, exact allocation/semantic parity "
        f"after {len(exceptions)} declared profile-only exception"
    )
    return 0


def main() -> int:
    options = arguments()
    with exclusive_evidence_locks(
        (options.evidence_4k, options.evidence_16k)
    ) as directories:
        options.evidence_4k, options.evidence_16k = directories
        return run(options)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
