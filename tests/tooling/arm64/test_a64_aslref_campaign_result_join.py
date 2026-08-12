#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exact-coverage negatives for the final ASLRef campaign promotion join."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import copy
import sqlite3
import tempfile
import tracemalloc
from pathlib import Path

import run_a64_aslref_campaign as campaign
from join_a64_aslref_campaign_results import (
    DEFAULT_MAX_PROMOTION_DATABASE_BYTES,
    PROMOTION_CAPACITY_REQUIRED_BYTES,
    PROMOTION_CONSERVATIVE_BYTES_PER_INPUT,
    build_promotions,
    install_official_results,
    publish_or_verify_promotion_output,
    require_promotion_capacity,
    summarize_promotion_bindings,
    tsv_payload,
    validate_output_target,
    verify_promotion_output,
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def expect_failure(action, message: str) -> None:
    try:
        action()
    except (sqlite3.DatabaseError, ValueError):
        return
    raise AssertionError(message)


def campaign_record(page: int, aggregate: str, ordinal: int) -> dict[str, object]:
    input_id = "input-contract-" + f"{ordinal:064x}"
    raw_id = "raw-contract-" + f"{ordinal:064x}"
    digest = f"{ordinal:064x}"
    _, binding_digest = campaign._digest_tuples([(input_id, raw_id)])
    _, result_digest = campaign._digest_tuples(
        [
            (
                input_id,
                raw_id,
                "shard-00000000",
                digest,
                digest,
                digest,
                digest,
            )
        ]
    )
    _, shard_digest = campaign._digest_tuples([("shard-00000000",)])
    return {
        "page": page,
        "index": {
            aggregate: {
                "execution_obligation_id": f"execution-{ordinal}",
                "allocation_status_id": f"allocation-{ordinal}",
                "semantic_input_id": f"semantic-{ordinal}",
                "validity_partition": "allocated-valid",
                "materialization_status": "materialized-execution-ready",
                "official_semantic_status": "pending-official-asl-execution",
                "representation_status": "representable",
                "representation_obligations": "none",
                "mapping_status": "exact-allocation",
                "allocation_behavior_complete": "0",
                "input_contract_count": "1",
                "raw_contract_id_count": "1",
            }
        },
        "result_count": 1,
        "aggregate_summaries": {
            aggregate: {
                "binding_count": 1,
                "binding_digest": binding_digest,
                "result_digest": result_digest,
                "shard_count": 1,
                "shard_digest": shard_digest,
            }
        },
        "plan_identity": {"sha256": digest},
        "report_identity": {"sha256": digest},
        "report": {
            "published_probe_result_binding_count": 1,
            "published_probe_result_bindings_sha256": digest,
        },
        "authority": {"join_model_digest": "a" * 64},
}


def result_row(ordinal: int) -> dict[str, str]:
    input_digest = f"{ordinal:064x}"
    shard = ordinal // 1024
    digest = f"{shard:064x}"
    return {
        "input_id": "input-contract-" + input_digest,
        "shard_id": f"shard-{shard:08d}",
        "input_results_sha256": digest,
    }


def populate_scale_database(
    path: Path, count: int, aggregate_count: int = 64
) -> sqlite3.Connection:
    database = sqlite3.connect(path)
    database.execute("PRAGMA journal_mode=OFF")
    database.execute("PRAGMA synchronous=OFF")
    campaign.configure_bounded_sqlite_storage(
        database, path, 256 * 1024 * 1024
    )
    database.execute(
        "CREATE TABLE contract_id("
        "identifier TEXT PRIMARY KEY, shard_id TEXT NOT NULL, "
        "raw_id TEXT NOT NULL)"
    )
    database.execute(
        "CREATE TABLE authoritative_contract_id("
        "identifier TEXT PRIMARY KEY) WITHOUT ROWID"
    )
    database.execute(
        "CREATE TABLE authoritative_binding("
        "aggregate_id TEXT NOT NULL, input_id TEXT NOT NULL, raw_id TEXT NOT NULL, "
        "PRIMARY KEY(aggregate_id, raw_id), "
        "UNIQUE(aggregate_id, input_id)) WITHOUT ROWID"
    )
    database.executemany(
        "INSERT INTO contract_id VALUES (?, ?, ?)",
        (
            (
                f"input-contract-{index:064x}",
                f"shard-{index // 1024:08d}",
                f"raw-contract-{index:064x}",
            )
            for index in range(count)
        ),
    )
    database.executemany(
        "INSERT INTO authoritative_contract_id VALUES (?)",
        ((f"input-contract-{index:064x}",) for index in range(count)),
    )
    database.executemany(
        "INSERT INTO authoritative_binding VALUES (?, ?, ?)",
        (
            (
                f"aggregate-{index % aggregate_count:020d}",
                f"input-contract-{index:064x}",
                f"raw-contract-{index:064x}",
            )
            for index in range(count)
        ),
    )
    database.execute(
        "CREATE INDEX authoritative_binding_by_input "
        "ON authoritative_binding(input_id, raw_id)"
    )
    campaign.require_exact_authoritative_id_set(database)
    database.execute("DROP TABLE contract_id")
    install_official_results(
        database, (result_row(index) for index in range(count))
    )
    return database


def disk_backed_scale_fixture(root: Path) -> None:
    peaks: list[int] = []
    database_sizes: list[int] = []
    for count in (4096, 65536):
        database_path = root / f"scale-{count}.sqlite3"
        database = populate_scale_database(database_path, count)
        temporary_directory = database.execute(
            "PRAGMA temp_store_directory"
        ).fetchone()
        require(
            temporary_directory is not None
            and Path(str(temporary_directory[0])).resolve() == root.resolve(),
            "promotion SQLite temporary storage escaped its checked root",
        )
        tracemalloc.start()
        summaries = summarize_promotion_bindings(database, count)
        _current, peak = tracemalloc.get_traced_memory()
        tracemalloc.stop()
        peaks.append(peak)
        database_sizes.append(database_path.stat().st_size)
        require(
            len(summaries) == 64
            and sum(int(row["binding_count"]) for row in summaries.values())
            == count,
            "disk-backed promotion scale summary differs",
        )
        database.close()
    require(
        peaks[1] <= peaks[0] + 2 * 1024 * 1024,
        "promotion Python memory grew with total input cardinality",
    )
    measured_bytes_per_input = (
        database_sizes[1] + 65536 - 1
    ) // 65536
    require(
        measured_bytes_per_input <= PROMOTION_CONSERVATIVE_BYTES_PER_INPUT
        and PROMOTION_CAPACITY_REQUIRED_BYTES
        <= DEFAULT_MAX_PROMOTION_DATABASE_BYTES,
        "promotion real-corpus database capacity model is not conservative",
    )
    print(
        "A64 promotion scale: "
        f"inputs=65536 aggregate_summaries=64 "
        f"database_bytes={database_sizes[1]} "
        f"measured_bytes_per_input={measured_bytes_per_input} "
        f"small_peak_bytes={peaks[0]} large_peak_bytes={peaks[1]}"
    )


def disk_backed_set_negatives(root: Path) -> None:
    database = populate_scale_database(root / "set-negative.sqlite3", 2, 2)
    try:
        database.execute(
            "DELETE FROM official_result WHERE input_id = ?",
            ("input-contract-" + "0" * 64,),
        )
        expect_failure(
            lambda: summarize_promotion_bindings(database, 1),
            "disk-backed promotion accepted a missing official result",
        )
        database.execute(
            "INSERT INTO official_result VALUES (?, ?)",
            (
                str(result_row(15)["input_id"]),
                str(result_row(15)["shard_id"]),
            ),
        )
        expect_failure(
            lambda: summarize_promotion_bindings(database, 2),
            "same-count unrelated disk-backed official result was accepted",
        )
    finally:
        database.close()

    duplicate = sqlite3.connect(":memory:")
    try:
        row = result_row(1)
        expect_failure(
            lambda: install_official_results(duplicate, iter((row, row))),
            "duplicate disk-backed official result was accepted",
        )
    finally:
        duplicate.close()

    inconsistent = sqlite3.connect(":memory:")
    try:
        first = result_row(0)
        second = result_row(1)
        second["input_results_sha256"] = "f" * 64
        expect_failure(
            lambda: install_official_results(
                inconsistent, iter((first, second))
            ),
            "one verified shard accepted inconsistent repeated digest fields",
        )
    finally:
        inconsistent.close()


def sqlite_hard_cap_fixture(root: Path) -> None:
    path = root / "hard-cap.sqlite3"
    database = sqlite3.connect(path)
    try:
        database.execute("PRAGMA journal_mode=OFF")
        database.execute("PRAGMA synchronous=OFF")
        campaign.configure_bounded_sqlite_storage(database, path, 8192)
        require(
            database.execute("PRAGMA journal_mode").fetchone()[0] == "off"
            and int(database.execute("PRAGMA synchronous").fetchone()[0]) == 0,
            "promotion disposable database journaling was not disabled",
        )
        database.execute("CREATE TABLE payload(value BLOB)")
        expect_failure(
            lambda: database.execute(
                "INSERT INTO payload VALUES (zeroblob(8192))"
            ),
            "promotion SQLite hard page cap accepted an oversized write",
        )
        database.rollback()
    finally:
        database.close()
    require(
        path.stat().st_size <= 8192
        and not path.with_name(path.name + "-journal").exists(),
        "promotion database cap escaped through its rollback journal",
    )


def main() -> int:
    aggregate_4k = "aggregate-00000000000000000001"
    aggregate_16k = "aggregate-00000000000000000002"
    aggregates = [
        {
            "aggregate_id": aggregate_4k,
            "guest_page_size": "4096",
            "validity_partition": "allocated-valid",
        },
        {
            "aggregate_id": aggregate_16k,
            "guest_page_size": "16384",
            "validity_partition": "allocated-valid",
        },
    ]
    campaigns = [
        campaign_record(4096, aggregate_4k, 1),
        campaign_record(16384, aggregate_16k, 2),
    ]
    rows = build_promotions(
        aggregates, campaigns, expected_allocated_valid_count=2
    )
    require(
        len(rows) == 2
        and all(row["allocation_behavior_complete"] == "1" for row in rows)
        and all(row["unresolved_requirements"] == "none" for row in rows),
        "exact two-page result join did not promote all eligible aggregates",
    )

    missing = copy.deepcopy(campaigns)
    missing[0]["aggregate_summaries"] = {}
    expect_failure(
        lambda: build_promotions(
            aggregates, missing, expected_allocated_valid_count=2
        ),
        "missing authoritative result binding was promoted",
    )

    replacement = copy.deepcopy(campaigns)
    summaries = replacement[0]["aggregate_summaries"]
    assert isinstance(summaries, dict)
    summary = summaries.pop(aggregate_4k)
    summaries["aggregate-" + "f" * 20] = summary
    expect_failure(
        lambda: build_promotions(
            aggregates, replacement, expected_allocated_valid_count=2
        ),
        "same-count unrelated official result replacement was promoted",
    )

    duplicate = copy.deepcopy(campaigns)
    duplicate[0]["aggregate_summaries"][aggregate_4k]["binding_count"] = 2
    expect_failure(
        lambda: build_promotions(
            aggregates, duplicate, expected_allocated_valid_count=2
        ),
        "duplicate authoritative result binding was promoted",
    )

    pending = copy.deepcopy(campaigns)
    pending[1]["index"][aggregate_16k]["official_semantic_status"] = (
        "blocked-typed-projection-obligations"
    )
    expect_failure(
        lambda: build_promotions(
            aggregates, pending, expected_allocated_valid_count=2
        ),
        "non-ready aggregate status was promoted",
    )

    mixed = copy.deepcopy(campaigns)
    mixed[1]["page"] = 4096
    expect_failure(
        lambda: build_promotions(
            aggregates, mixed, expected_allocated_valid_count=2
        ),
        "two campaigns with one page profile were promoted",
    )

    payload = tsv_payload(rows)
    with tempfile.TemporaryDirectory(prefix="a64-result-join-") as temporary:
        root = Path(temporary).resolve()
        disk_backed_scale_fixture(root)
        disk_backed_set_negatives(root)
        sqlite_hard_cap_fixture(root)
        output = root / "promotions.tsv"
        capacity_root = root / "capacity"
        capacity_work = capacity_root / "work"
        capacity_work.mkdir(parents=True)
        (capacity_work / "authority.sqlite3").write_bytes(b"x" * 100)
        original_disk_usage = campaign.shutil.disk_usage
        required = int(1.25 * 1024**3) + 4096 + 500 - 100
        campaign.shutil.disk_usage = lambda _path: type(
            "DiskUsage", (), {"free": required}
        )()
        try:
            require_promotion_capacity(
                capacity_root, capacity_work, 1.25, 4096, 500
            )
            campaign.shutil.disk_usage = lambda _path: type(
                "DiskUsage", (), {"free": required - 1}
            )()
            expect_failure(
                lambda: require_promotion_capacity(
                    capacity_root, capacity_work, 1.25, 4096, 500
                ),
                "promotion reserve/headroom accepted one byte too little",
            )
        finally:
            campaign.shutil.disk_usage = original_disk_usage
        validate_output_target(output)
        require(
            publish_or_verify_promotion_output(
                output, payload, rows, verify_only=False
            )["row_count"]
            == len(rows),
            "fresh promotion TSV did not publish and verify",
        )
        require(
            publish_or_verify_promotion_output(
                output, payload, rows, verify_only=True
            )["row_count"]
            == len(rows),
            "promotion verify-only result differs",
        )

        output.write_bytes(payload + b"\n")
        expect_failure(
            lambda: verify_promotion_output(output, rows),
            "mutated promotion TSV was accepted",
        )
        output.write_bytes(payload)
        protected_root = root / "campaign-root"
        protected_root.mkdir()
        expect_failure(
            lambda: validate_output_target(
                protected_root / "promotion.tsv",
                forbidden_directories=[protected_root],
            ),
            "promotion output inside a campaign root was accepted",
        )
        expect_failure(
            lambda: validate_output_target(
                output,
                forbidden_files=[output],
            ),
            "promotion output aliasing a protected input was accepted",
        )
        missing = root / "missing.tsv"
        expect_failure(
            lambda: publish_or_verify_promotion_output(
                missing, payload, rows, verify_only=True
            ),
            "verify-only mode created a missing promotion TSV",
        )

    print("A64 ASLRef campaign final result-join fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
