#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Publish complete two-profile aggregate evidence and its bound identity."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import json
import os
import sys
from pathlib import Path
from typing import Mapping, Sequence

from a64_aggregate_manifest_identity import (
    aggregate_manifest_identity,
    canonical_json,
    read_manifest_identity,
    validate_manifest_identity,
)
from a64_bounded_generation import (
    BoundedCommandRunner,
    BoundedWorkspace,
    add_bounded_generation_arguments,
    workspace_from_options,
)
from a64_raw_evidence_files import (
    atomic_publish_bytes,
    exclusive_evidence_locks,
    freeze_read_inputs,
    paths_alias,
    publication_target_locks,
    raw_evidence_files,
    recheck_read_inputs,
    require_regular_raw_file,
    validate_publication_topology,
    validate_raw_profile,
    validate_producer_authority,
)
from a64_llvm_oracle import find_llvm_mc, require_pinned_llvm
from aggregate_a64_raw_instruction_evidence import OUTPUT_COLUMNS, aggregate_id
from generate_a64_instruction_decode_partitions import COLUMNS as DECODE_COLUMNS
from generate_a64_instruction_required_variants import COLUMNS as REQUIRED_COLUMNS
from validate_a64_instruction_aggregate_manifest import (
    PROFILE_EXCEPTION_COLUMNS,
    compare_profiles,
    validate_profile_exceptions,
    validate_rows,
)


FULL_PROFILE_COUNTS = {"4096": 16084, "16384": 16085}


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument("--full-4k-manifest", type=Path, required=True)
    parser.add_argument("--full-16k-manifest", type=Path, required=True)
    parser.add_argument("--evidence-4k", type=Path, required=True)
    parser.add_argument("--evidence-16k", type=Path, required=True)
    parser.add_argument(
        "--required-variants",
        type=Path,
        default=repository
        / "tests/unit/arm64/a64_instruction_required_variants.tsv",
    )
    parser.add_argument(
        "--decode-partitions",
        type=Path,
        default=repository
        / "tests/unit/arm64/a64_instruction_decode_partitions.tsv",
    )
    parser.add_argument(
        "--profile-exceptions",
        type=Path,
        default=repository
        / "tests/unit/arm64/a64_instruction_profile_exceptions.tsv",
    )
    parser.add_argument("--llvm-mc", type=Path)
    parser.add_argument("--decode-chunk-size", type=int, default=20000)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--identity-output", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=int, default=1800)
    add_bounded_generation_arguments(parser)
    return parser.parse_args()


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != OUTPUT_COLUMNS:
            raise ValueError(f"{path}: aggregate columns differ")
        rows = list(reader)
    if not rows:
        raise ValueError(f"{path}: aggregate manifest is empty")
    return rows


def tsv_payload(rows: list[dict[str, str]]) -> bytes:
    import io

    output = io.StringIO(newline="")
    writer = csv.DictWriter(
        output, fieldnames=OUTPUT_COLUMNS, delimiter="\t", lineterminator="\n"
    )
    writer.writeheader()
    writer.writerows(rows)
    return output.getvalue().encode("utf-8")


def full_publication_rows(
    rows_4k: list[dict[str, str]],
    rows_16k: list[dict[str, str]],
    expected_producers: Mapping[str, set[str]],
) -> list[dict[str, str]]:
    """Validate the exact complete profile union and return canonical rows."""
    profiles = {"4096": rows_4k, "16384": rows_16k}
    producer_sets: dict[str, set[str]] = {}
    stable_id_sets: dict[str, set[str]] = {}
    profile_ids: dict[str, set[str]] = {}
    for page, rows in profiles.items():
        wrong_pages = sorted(
            {row["guest_page_size"] for row in rows if row["guest_page_size"] != page}
        )
        if wrong_pages:
            raise ValueError(
                f"full {page} source contains cross-page rows: {wrong_pages}"
            )
        identifiers = [row["aggregate_id"] for row in rows]
        if len(identifiers) != len(set(identifiers)):
            raise ValueError(f"full {page} source contains duplicate aggregate IDs")
        mismatched = [
            row["aggregate_id"]
            for row in rows
            if row["aggregate_id"] != aggregate_id(row)
        ]
        if mismatched:
            raise ValueError(
                f"full {page} source contains an invalid aggregate identity: "
                f"{mismatched[:1]}"
            )
        expected_order = sorted(
            rows, key=lambda row: (row["stable_id"], row["aggregate_id"])
        )
        if rows != expected_order:
            raise ValueError(f"full {page} source is not in canonical source order")
        expected_count = FULL_PROFILE_COUNTS[page]
        if len(rows) != expected_count:
            raise ValueError(
                f"full {page} source has {len(rows)} rows; expected "
                f"{expected_count}"
            )
        producers = {row["producer"] for row in rows}
        expected_page_producers = expected_producers.get(page)
        if expected_page_producers is None:
            raise ValueError(f"full {page} source lacks a producer authority")
        validate_producer_authority(expected_page_producers, page)
        if producers != expected_page_producers:
            missing = sorted(expected_page_producers - producers)
            extra = sorted(producers - expected_page_producers)
            raise ValueError(
                f"full {page} source producer set differs from raw evidence; "
                f"missing={missing[:3]}, extra={extra[:3]}"
            )
        producer_sets[page] = producers
        stable_id_sets[page] = {row["stable_id"] for row in rows}
        profile_ids[page] = set(identifiers)

    if producer_sets["4096"] != producer_sets["16384"]:
        raise ValueError("full profile producer sets differ")
    if stable_id_sets["4096"] != stable_id_sets["16384"]:
        raise ValueError("full profile stable-ID sets differ")
    overlap = profile_ids["4096"] & profile_ids["16384"]
    if overlap:
        raise ValueError(
            "full profile aggregate identity sets overlap: "
            + ",".join(sorted(overlap)[:3])
        )
    combined = sorted(
        [*rows_4k, *rows_16k], key=lambda row: row["aggregate_id"]
    )
    expected_total = sum(FULL_PROFILE_COUNTS.values())
    if len(combined) != expected_total or len(
        {row["aggregate_id"] for row in combined}
    ) != expected_total:
        raise ValueError("full publication identity set/cardinality differs")
    return combined


def raw_producer_authority(
    directory: Path, page: str
) -> tuple[set[str], list[Path]]:
    entries = raw_evidence_files(directory)
    producers = {entry.producer for entry in entries}
    validate_producer_authority(producers, page)
    for entry in entries:
        validate_raw_profile(entry, page)
    return producers, [entry.path for entry in entries]


def require_exact_raw_derivation(
    supplied: list[dict[str, str]],
    derived: list[dict[str, str]],
    page: str,
) -> None:
    if supplied == derived:
        return
    supplied_ids = {row["aggregate_id"] for row in supplied}
    derived_ids = {row["aggregate_id"] for row in derived}
    raise ValueError(
        f"full {page} source does not exactly derive from supplied raw "
        f"evidence; missing={sorted(derived_ids - supplied_ids)[:3]}, "
        f"extra={sorted(supplied_ids - derived_ids)[:3]}"
    )


def derive_profile_manifest(
    evidence: Path,
    output: Path,
    options: argparse.Namespace,
    runner: BoundedCommandRunner,
) -> list[dict[str, str]]:
    script = Path(__file__).with_name(
        "aggregate_a64_raw_instruction_evidence.py"
    )
    command = [
        sys.executable,
        "-E",
        "-s",
        "-B",
        str(script),
        "--evidence-directory",
        str(evidence),
        "--required-variants",
        str(options.required_variants),
        "--decode-partitions",
        str(options.decode_partitions),
        "--output",
        str(output),
        "--decode-chunk-size",
        str(options.decode_chunk_size),
        "--assume-evidence-lock-held",
    ]
    if options.llvm_mc is not None:
        command.extend(("--llvm-mc", str(options.llvm_mc)))
    environment = {
        name: value
        for name, value in os.environ.items()
        if not name.startswith("PYTHON")
    }
    completed = runner.run(
        command,
        check=False,
        text=True,
        capture_output=True,
        env=environment,
    )
    if completed.returncode != 0:
        raise ValueError(
            f"cannot derive full aggregate from {evidence}: "
            f"{(completed.stderr + completed.stdout).strip()}"
        )
    return read_tsv(output)


def validate_full_publication_paths(
    full_4k: Path,
    full_16k: Path,
    output: Path,
    identity_output: Path,
    read_inputs: Sequence[Path] = (),
) -> tuple[Path, Path]:
    files, _directories, sources = validate_publication_topology(
        (output, identity_output),
        read_inputs=(full_4k, full_16k, *read_inputs),
    )
    if paths_alias(sources[0], sources[1]):
        raise ValueError("full profile inputs alias each other")
    return files


def _run_full_locked(
    options: argparse.Namespace,
    workspace: BoundedWorkspace,
    runner: BoundedCommandRunner,
) -> tuple[int, int]:
    if (
        options.full_4k_manifest is None
        or options.full_16k_manifest is None
        or options.evidence_4k is None
        or options.evidence_16k is None
    ):
        raise ValueError(
            "full publication requires --full-4k-manifest, "
            "--full-16k-manifest, --evidence-4k, and --evidence-16k"
        )
    producers_4k, raw_paths_4k = raw_producer_authority(
        options.evidence_4k, "4096"
    )
    producers_16k, raw_paths_16k = raw_producer_authority(
        options.evidence_16k, "16384"
    )
    tool = options.llvm_mc
    if tool is None:
        raise ValueError("bounded full publication lacks its LLVM tool")
    require_pinned_llvm(tool, runner.run)
    validate_full_publication_paths(
        options.full_4k_manifest,
        options.full_16k_manifest,
        options.output,
        options.identity_output,
        (
            options.required_variants,
            options.decode_partitions,
            options.profile_exceptions,
            *raw_paths_4k,
            *raw_paths_16k,
        ),
    )
    read_identities = freeze_read_inputs(
        (
            options.full_4k_manifest,
            options.full_16k_manifest,
            options.required_variants,
            options.decode_partitions,
            options.profile_exceptions,
            tool,
            *raw_paths_4k,
            *raw_paths_16k,
        )
    )
    expected_raw_paths = {
        "4096": {path.resolve() for path in raw_paths_4k},
        "16384": {path.resolve() for path in raw_paths_16k},
    }

    def recheck_publication_inputs() -> None:
        for page, directory in (
            ("4096", options.evidence_4k),
            ("16384", options.evidence_16k),
        ):
            entries = raw_evidence_files(directory)
            producers = {entry.producer for entry in entries}
            validate_producer_authority(producers, page)
            if {entry.path.resolve() for entry in entries} != expected_raw_paths[page]:
                raise ValueError(
                    f"full {page} raw evidence file set changed during publication"
                )
        recheck_read_inputs(read_identities)

    producer_authority = {
        "4096": producers_4k,
        "16384": producers_16k,
    }
    rows_4k = read_tsv(options.full_4k_manifest)
    rows_16k = read_tsv(options.full_16k_manifest)
    rows = full_publication_rows(
        rows_4k, rows_16k, producer_authority
    )

    temporary = workspace.require_path() / "derivation"
    temporary.mkdir()
    derived_4k = derive_profile_manifest(
        options.evidence_4k,
        temporary / "derived-4k.tsv",
        options,
        runner,
    )
    derived_16k = derive_profile_manifest(
        options.evidence_16k,
        temporary / "derived-16k.tsv",
        options,
        runner,
    )
    require_exact_raw_derivation(rows_4k, derived_4k, "4096")
    require_exact_raw_derivation(rows_16k, derived_16k, "16384")

    required = read_tsv_with_columns(options.required_variants, REQUIRED_COLUMNS)
    decode = read_tsv_with_columns(options.decode_partitions, DECODE_COLUMNS)
    exceptions = read_tsv_with_columns(
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

    identity = aggregate_manifest_identity(rows, OUTPUT_COLUMNS)
    publication_outputs = (
        (options.output, tsv_payload(rows)),
        (
            options.identity_output,
            (canonical_json(identity) + "\n").encode("utf-8"),
        ),
    )

    def validate_staged(staged: Sequence[tuple[Path, Path]]) -> None:
        if len(staged) != len(publication_outputs):
            raise ValueError("staged full publication cardinality differs")
        for (_original, payload), (target, temporary) in zip(
            publication_outputs, staged
        ):
            if temporary.read_bytes() != payload:
                raise ValueError(f"staged full publication differs: {target}")

    def validate_published() -> None:
        for target, payload in publication_outputs:
            if target.read_bytes() != payload:
                raise ValueError(f"published full publication differs: {target}")

    atomic_publish_bytes(
        publication_outputs,
        validate_staged=validate_staged,
        before_replace=recheck_publication_inputs,
        validate_published=validate_published,
    )
    published_rows = read_tsv(options.output)
    if published_rows != rows:
        raise ValueError("full publication manifest changed after replacement")
    validate_manifest_identity(
        published_rows,
        OUTPUT_COLUMNS,
        read_manifest_identity(options.identity_output),
    )
    return len(rows_4k), len(rows_16k)


def run_full(options: argparse.Namespace) -> tuple[int, int]:
    if options.evidence_4k is None or options.evidence_16k is None:
        raise ValueError("full publication requires both evidence directories")
    with exclusive_evidence_locks(
        (options.evidence_4k, options.evidence_16k)
    ) as directories:
        options.evidence_4k, options.evidence_16k = directories
        raw_paths = (
            *(entry.path for entry in raw_evidence_files(options.evidence_4k)),
            *(entry.path for entry in raw_evidence_files(options.evidence_16k)),
        )
        tool = find_llvm_mc(options.llvm_mc).resolve()
        options.llvm_mc = tool
        outputs = validate_full_publication_paths(
            options.full_4k_manifest,
            options.full_16k_manifest,
            options.output,
            options.identity_output,
            (
                options.required_variants,
                options.decode_partitions,
                options.profile_exceptions,
                tool,
                *raw_paths,
            ),
        )
        with publication_target_locks(outputs) as locked_outputs:
            options.output, options.identity_output = locked_outputs
            with workspace_from_options(
                options,
                source_root=Path(__file__).resolve().parents[3],
                publication_targets=locked_outputs,
                prefix="a64-full-publication-derivation-",
            ) as workspace:
                runner = BoundedCommandRunner(
                    workspace, options.timeout_seconds
                )
                return _run_full_locked(options, workspace, runner)


def read_tsv_with_columns(
    path: Path, columns: list[str]
) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != columns:
            raise ValueError(f"{path}: unexpected columns {reader.fieldnames!r}")
        rows = list(reader)
    if not rows:
        raise ValueError(f"{path}: contains no rows")
    return rows


def main() -> int:
    options = arguments()
    count_4k, count_16k = run_full(options)
    print(
        f"A64 full aggregate publication: validated {count_4k} 4K and "
        f"{count_16k} 16K rows; wrote canonical manifest and frozen "
        "identity"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=__import__("sys").stderr)
        raise SystemExit(1)
