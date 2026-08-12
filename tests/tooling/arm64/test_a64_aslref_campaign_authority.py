#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Adversarial authoritative-input fixtures for ASLRef campaigns."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import base64
import copy
import csv
import gzip
import hashlib
import json
import os
import sqlite3
import tempfile
from pathlib import Path
from typing import Mapping, Sequence

from a64_input_materialization import AGGREGATE_SCHEMA, wire_json
from aggregate_a64_raw_instruction_evidence import OUTPUT_COLUMNS
from join_a64_input_materializations import (
    AggregateAccumulator,
    CONTRACT_INDEX_SCHEMA,
    CONTRACT_MANIFEST_COLUMNS,
    CONTRACT_MANIFEST_SCHEMA,
    CONTRIBUTION_CHECKPOINT_FOOTER_SCHEMA,
    CONTRIBUTION_CHECKPOINT_HEADER_SCHEMA,
    DECLARED_COLUMNS,
    INDEX_COLUMNS,
    verify_contribution_checkpoint,
)
import run_a64_aslref_campaign as campaign
from test_a64_input_materialization import event_stream, materialization


AGGREGATES = (
    "aggregate-00000000000000000001",
    "aggregate-00000000000000000002",
)
JOIN_MODEL = "a" * 64


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def expect_failure(action, message: str) -> None:
    try:
        action()
    except (OSError, ValueError):
        return
    raise AssertionError(message)


def write_tsv(
    path: Path, columns: Sequence[str], rows: Sequence[Mapping[str, str]]
) -> None:
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(
            output, fieldnames=columns, delimiter="\t", lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)


def framed_digest(lines: Sequence[bytes]) -> str:
    digest = hashlib.sha256()
    for line in lines:
        digest.update(len(line).to_bytes(8, "big"))
        digest.update(line)
    return digest.hexdigest()


def contribution(
    aggregate_id: str, ordinal: int, sort_byte: int
) -> dict[str, object]:
    fields = [f"fixture-{ordinal}" for _ in DECLARED_COLUMNS]
    fields[-1] = "1"
    declaration = "\0".join(fields).encode("utf-8") + b"\n"
    return {
        "schema": CONTRACT_INDEX_SCHEMA,
        "aggregate_id": aggregate_id,
        "sort_key": bytes([sort_byte]).hex(),
        "declaration_base64": base64.b64encode(declaration).decode("ascii"),
        "raw_contract_id": "raw-contract-" + f"{ordinal:064x}",
        "input_materialization_id": "input-contract-" + f"{ordinal:064x}",
        "raw_occurrences": "1",
        "representation_obligations": [],
    }


def write_checkpoint(
    path: Path, producer: str, rows: Sequence[Mapping[str, object]]
):
    header = {
        "schema": CONTRIBUTION_CHECKPOINT_HEADER_SCHEMA,
        "producer": producer,
        "source_name": f"{producer}.inputs.jsonl.gz",
        "source_sha256": hashlib.sha256(producer.encode()).hexdigest(),
        "source_size": "1",
        "join_model_digest": JOIN_MODEL,
        "guest_page_size": "4096",
    }
    ordered = sorted(
        rows,
        key=lambda row: (
            bytes.fromhex(str(row["sort_key"])),
            str(row["aggregate_id"]),
            str(row["raw_contract_id"]),
        ),
    )
    encoded_rows = [
        (campaign.canonical_json(dict(row)) + "\n").encode("utf-8")
        for row in ordered
    ]
    footer = {
        "schema": CONTRIBUTION_CHECKPOINT_FOOTER_SCHEMA,
        "contribution_count": str(len(ordered)),
        "contribution_digest": framed_digest(encoded_rows),
    }
    with gzip.open(path, "wb") as output:
        output.write((campaign.canonical_json(header) + "\n").encode("utf-8"))
        for line in encoded_rows:
            output.write(line)
        output.write((campaign.canonical_json(footer) + "\n").encode("utf-8"))
    return verify_contribution_checkpoint(path)


def aggregate_summary(rows: Sequence[Mapping[str, object]]) -> AggregateAccumulator:
    accumulator = AggregateAccumulator()
    for row in sorted(
        rows,
        key=lambda item: (
            bytes.fromhex(str(item["sort_key"])),
            str(item["raw_contract_id"]),
        ),
    ):
        accumulator.add(row)
    accumulator.finish()
    return accumulator


def fixture(root: Path, mutation: str = "none") -> dict[str, object]:
    base_rows = [
        contribution(AGGREGATES[0], 1, 0x10),
        contribution(AGGREGATES[1], 5, 0x15),
        contribution(AGGREGATES[0], 2, 0x20),
        contribution(AGGREGATES[1], 6, 0x25),
        contribution(AGGREGATES[0], 3, 0x30),
        contribution(AGGREGATES[0], 4, 0x40),
    ]
    checkpoint_rows = copy.deepcopy(base_rows)
    if mutation == "cross-aggregate-swap":
        first = checkpoint_rows[0]
        second = checkpoint_rows[1]
        for name in ("raw_contract_id", "input_materialization_id"):
            first[name], second[name] = second[name], first[name]
    elif mutation == "duplicate":
        checkpoint_rows[2]["input_materialization_id"] = checkpoint_rows[0][
            "input_materialization_id"
        ]

    by_producer = {
        "producer-a": [checkpoint_rows[index] for index in (0, 1, 4)],
        "producer-b": [checkpoint_rows[index] for index in (2, 3, 5)],
    }
    manifest_rows: list[dict[str, str]] = []
    checkpoints: list[Path] = []
    for producer, rows in by_producer.items():
        checkpoint = root / f"{producer}.contributions.jsonl.gz"
        info = write_checkpoint(checkpoint, producer, rows)
        checkpoints.append(checkpoint)
        manifest_rows.append(
            {
                "schema": CONTRACT_MANIFEST_SCHEMA,
                "producer": info.producer,
                "checkpoint_file": checkpoint.name,
                "checkpoint_sha256": info.checkpoint_sha256,
                "checkpoint_size": str(info.checkpoint_size),
                "source_name": info.source_name,
                "source_sha256": info.source_sha256,
                "source_size": str(info.source_size),
                "join_model_digest": info.join_model_digest,
                "guest_page_size": str(info.guest_page_size),
                "contribution_count": str(info.contribution_count),
                "contribution_digest": info.contribution_digest,
            }
        )
    contract_manifest = root / "contract-manifest.tsv"
    write_tsv(contract_manifest, CONTRACT_MANIFEST_COLUMNS, manifest_rows)

    aggregate_rows: list[dict[str, str]] = []
    index_rows: list[dict[str, str]] = []
    for index, aggregate_id in enumerate(AGGREGATES, 1):
        rows = [row for row in base_rows if row["aggregate_id"] == aggregate_id]
        summary = aggregate_summary(rows)
        aggregate = {column: "fixture" for column in OUTPUT_COLUMNS}
        aggregate.update(
            {
                "aggregate_id": aggregate_id,
                "guest_page_size": "4096",
                "validity_partition": "allocated-valid",
                "decode_partition_id": f"decode-fixture-{index}",
                "declared_count": str(summary.declared_count),
                "declared_digest": summary.declared_digest.hexdigest(),
                "observed_digest": hashlib.sha256(
                    aggregate_id.encode()
                ).hexdigest(),
            }
        )
        aggregate_rows.append(aggregate)
        index_row = {column: "fixture" for column in INDEX_COLUMNS}
        index_row.update(
            {
                "schema": AGGREGATE_SCHEMA,
                "aggregate_id": aggregate_id,
                "execution_obligation_id": f"execution-fixture-{index}",
                "allocation_status_id": f"allocation-fixture-{index}",
                "semantic_input_id": f"semantic-fixture-{index}",
                "decode_partition_id": aggregate["decode_partition_id"],
                "validity_partition": "allocated-valid",
                "materialization_status": "materialized-execution-ready",
                "official_semantic_status": "pending-official-asl-execution",
                "input_contract_count": str(summary.contract_count),
                "raw_contract_id_count": str(summary.contract_count),
                "raw_contract_id_digest": summary.raw_id_digest.hexdigest(),
                "input_materialization_id_count": str(summary.contract_count),
                "input_materialization_id_digest": (
                    summary.input_id_digest.hexdigest()
                ),
                "representation_status": "representable",
                "representation_obligations": "none",
                "declared_count": aggregate["declared_count"],
                "declared_digest": aggregate["declared_digest"],
                "observed_digest": aggregate["observed_digest"],
                "mapping_status": "exact-allocation",
                "allocation_behavior_complete": "0",
            }
        )
        index_rows.append(index_row)

    aggregate_manifest = root / "aggregates.tsv"
    write_tsv(aggregate_manifest, OUTPUT_COLUMNS, aggregate_rows)
    materialization_index = root / "materialization-index.tsv"
    write_tsv(materialization_index, INDEX_COLUMNS, index_rows)
    return {
        "aggregate_manifest": aggregate_manifest,
        "materialization_index": materialization_index,
        "contract_manifest": contract_manifest,
        "checkpoints": checkpoints,
        "input_ids": {
            str(row["input_materialization_id"]) for row in base_rows
        },
        "input_raw_ids": {
            str(row["input_materialization_id"]): str(row["raw_contract_id"])
            for row in base_rows
        },
    }


def load(value: Mapping[str, object], ids: set[str] | None = None):
    database = sqlite3.connect(":memory:")
    database.execute(
        "CREATE TABLE contract_id(identifier TEXT PRIMARY KEY, "
        "shard_id TEXT NOT NULL, raw_id TEXT NOT NULL)"
    )
    for identifier in sorted(ids if ids is not None else value["input_ids"]):
        raw_id = value["input_raw_ids"].get(
            identifier, "raw-contract-" + "f" * 64
        )
        database.execute(
            "INSERT INTO contract_id VALUES (?, 'shard-00000000', ?)",
            (identifier, raw_id),
        )
    try:
        return campaign.load_authoritative_input(
            guest_page_size=4096,
            aggregate_manifest=Path(value["aggregate_manifest"]),
            materialization_index=Path(value["materialization_index"]),
            contract_manifests=[Path(value["contract_manifest"])],
            database=database,
        )
    finally:
        database.close()


def input_mutation_negatives(root: Path) -> None:
    original = materialization(4096)
    stream = event_stream([original])
    mutated = json.loads(stream.splitlines()[0])
    mutated["payload"]["words"][0]["word"] = "91000800"
    lines = stream.splitlines()
    lines[0] = wire_json(mutated)
    payload_path = root / "payload-mutated.jsonl.gz"
    with gzip.open(payload_path, "wt", encoding="utf-8", newline="") as output:
        output.write("\n".join(lines) + "\n")
    expect_failure(
        lambda: campaign.scan_shard_input(payload_path, guest_page_size=4096),
        "payload mutation retaining its content ID was accepted",
    )

    mixed_path = root / "mixed-page.jsonl.gz"
    with gzip.open(mixed_path, "wt", encoding="utf-8", newline="") as output:
        output.write(event_stream([materialization(4096), materialization(16384)]))
    expect_failure(
        lambda: campaign.scan_shard_input(mixed_path, guest_page_size=4096),
        "mixed-page campaign shard was accepted",
    )


def authority_capacity_boundary(root: Path) -> None:
    local = root / "authority-capacity-local"
    work = local / "work"
    work.mkdir(parents=True)
    (work / "authority.sqlite3").write_bytes(b"d" * 100)
    database_cap = 4096
    scratch_cap = 500
    reserve = 1.5
    required = int(reserve * 1024**3) + database_cap + scratch_cap - 100
    free = required
    original_disk_usage = campaign.shutil.disk_usage

    def disk_usage(_path):
        return type("DiskUsage", (), {"free": free})()

    try:
        campaign.shutil.disk_usage = disk_usage
        campaign.require_authority_capacity(
            local, work, reserve, database_cap, scratch_cap
        )
        free = required - 1
        expect_failure(
            lambda: campaign.require_authority_capacity(
                local, work, reserve, database_cap, scratch_cap
            ),
            "authority capacity accepted one byte below reserve/headroom",
        )
    finally:
        campaign.shutil.disk_usage = original_disk_usage


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="a64-campaign-authority-") as temporary:
        root = Path(temporary)
        exact_root = root / "exact"
        exact_root.mkdir()
        exact = fixture(exact_root)
        authority = load(exact)
        require(
            authority["input_contract_count"] == 6
            and authority["aggregate_input_binding_count"] == 6,
            "interleaved multi-producer authoritative join did not pass exactly",
        )

        replacement_ids = set(exact["input_ids"])
        replacement_ids.remove(min(replacement_ids))
        replacement_ids.add("input-contract-" + "f" * 64)
        expect_failure(
            lambda: load(exact, replacement_ids),
            "same-count unrelated campaign input replacement was accepted",
        )

        changed_raw = dict(exact)
        changed_raw["input_raw_ids"] = dict(exact["input_raw_ids"])
        first_input = min(changed_raw["input_raw_ids"])
        changed_raw["input_raw_ids"][first_input] = "raw-contract-" + "e" * 64
        expect_failure(
            lambda: load(changed_raw),
            "input ID with a different derived raw semantic binding was accepted",
        )

        swap_root = root / "cross-aggregate-swap"
        swap_root.mkdir()
        expect_failure(
            lambda: load(fixture(swap_root, "cross-aggregate-swap")),
            "cross-aggregate input/raw identity swap was accepted",
        )

        duplicate_root = root / "duplicate"
        duplicate_root.mkdir()
        expect_failure(
            lambda: load(fixture(duplicate_root, "duplicate")),
            "duplicate per-aggregate input identity was accepted",
        )

        drift_root = root / "merge-drift"
        drift_root.mkdir()
        drift_fixture = fixture(drift_root)
        original_merge = campaign.merge_checkpoint_contributions

        def drifting_merge(paths, **kwargs):
            yield from original_merge(paths, **kwargs)
            with Path(paths[0]).open("ab") as output:
                output.write(b"drift")

        try:
            campaign.merge_checkpoint_contributions = drifting_merge
            expect_failure(
                lambda: load(drift_fixture),
                "checkpoint mutation during the authority merge was accepted",
            )
        finally:
            campaign.merge_checkpoint_contributions = original_merge

        symlink_root = root / "symlink"
        symlink_root.mkdir()
        symlink_fixture = fixture(symlink_root)
        checkpoint = Path(symlink_fixture["checkpoints"][0])
        target = checkpoint.with_name(checkpoint.name + ".target")
        os.rename(checkpoint, target)
        os.symlink(target.name, checkpoint)
        expect_failure(
            lambda: load(symlink_fixture),
            "same-content symlinked authoritative checkpoint was accepted",
        )

        input_mutation_negatives(root)
        authority_capacity_boundary(root)

    print("A64 ASLRef campaign authoritative-input fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
