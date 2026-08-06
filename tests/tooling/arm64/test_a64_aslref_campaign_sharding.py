#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded sharding, durability, resume, and mutation fixtures."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import copy
import csv
import fcntl
import gzip
import json
import os
import resource
import sqlite3
import tempfile
import threading
import time
import tracemalloc
from pathlib import Path

import a64_aslref_campaign_sharding as sharding
import run_a64_aslref_campaign as campaign
from a64_raw_evidence_files import evidence_quiescence_lock_path
from a64_input_materialization import raw_contract_id
from join_a64_input_materializations import (
    CONTRACT_MANIFEST_COLUMNS,
    CONTRACT_MANIFEST_SCHEMA,
)
from test_a64_input_materialization import event_stream, finish, materialization


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def expect_failure(action, message: str) -> None:
    try:
        action()
    except (OSError, ValueError):
        return
    raise AssertionError(message)


def records(count: int = 3, page: int = 4096) -> list[dict[str, object]]:
    base = materialization(page)
    result = [base]
    for index in range(1, count):
        record = copy.deepcopy(base)
        identity = record["identity"]
        assert isinstance(identity, dict)
        identity["case"] = f"exact concrete input {index}"
        identity["line"] = f"{0x11 + index:08x}"
        result.append(finish(record))
    return result


def write_source(path: Path, values: list[dict[str, object]]) -> None:
    with path.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as output:
            output.write(event_stream(values).encode("utf-8"))


def write_contract_manifest(path: Path, source: Path) -> None:
    identity = campaign.file_identity(source)
    row = {name: "fixture" for name in CONTRACT_MANIFEST_COLUMNS}
    row.update(
        {
            "schema": CONTRACT_MANIFEST_SCHEMA,
            "source_name": source.name,
            "source_sha256": str(identity["sha256"]),
            "source_size": str(identity["size"]),
        }
    )
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=CONTRACT_MANIFEST_COLUMNS,
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerow(row)


def create_authority_tables(
    database: sqlite3.Connection,
    values: list[dict[str, object]],
) -> None:
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
    for index, record in enumerate(values):
        identifier = str(record["input_contract_id"])
        raw_id = raw_contract_id(record)
        database.execute(
            "INSERT INTO authoritative_contract_id VALUES (?)", (identifier,)
        )
        database.execute(
            "INSERT INTO authoritative_binding VALUES (?, ?, ?)",
            (f"aggregate-{index:020d}", identifier, raw_id),
        )
    database.execute(
        "CREATE INDEX authoritative_binding_by_input "
        "ON authoritative_binding(input_id, raw_id)"
    )


def fake_authority(
    database: sqlite3.Connection,
    values: list[dict[str, object]],
    frozen: Path,
    manifest: Path,
) -> dict[str, object]:
    create_authority_tables(database, values)
    frozen_identity = campaign.file_identity(frozen)
    return {
        "schema": "uml.a64.aslref-authoritative-input/v1",
        "guest_page_size": 4096,
        "aggregate_manifest": frozen_identity,
        "aggregate_manifest_identity": {"fixture": True},
        "aggregate_manifest_identity_file": frozen_identity,
        "materialization_index": frozen_identity,
        "contract_manifests": [campaign.file_identity(manifest)],
        "checkpoint_files": [frozen_identity],
        "join_model_digest": "1" * 64,
        "page_aggregate_count": len(values),
        "eligible_aggregate_count": len(values),
        "input_contract_count": len(values),
        "input_contract_ids_sha256": "2" * 64,
        "aggregate_input_binding_count": len(values),
        "aggregate_input_binding_sha256": "3" * 64,
        "checkpoint_count": 1,
        "checkpoint_manifest_sha256": "4" * 64,
    }


def fixture_options(
    root: Path,
    source: Path,
    manifest: Path,
    frozen: Path,
    *,
    output_name: str = "shards",
) -> argparse.Namespace:
    local = root / "local"
    local.mkdir(exist_ok=True)
    return argparse.Namespace(
        evidence_directory=[],
        input_jsonl=[source],
        guest_page_size=4096,
        aggregate_manifest=frozen,
        aggregate_manifest_identity=frozen,
        materialization_index=frozen,
        contract_manifest=[manifest],
        output_directory=root / output_name,
        local_temp_root=local,
        expected_total_contracts=3,
        minimum_output_free_gib=0.0,
        minimum_local_free_gib=0.0,
        maximum_shard_bytes=1024 * 1024,
        maximum_metadata_bytes=1024 * 1024,
        maximum_database_bytes=16 * 1024 * 1024,
        maximum_merge_scratch_bytes=16 * 1024 * 1024,
        directory_sync_policy="record-unsupported",
    )


def deterministic_writer_fixture(root: Path) -> None:
    values = records(2)
    outputs: list[bytes] = []
    for name in ("first", "second"):
        output = root / name
        staging = output / ".staging"
        staging.mkdir(parents=True)
        writer = sharding.DeterministicShardWriter(
            staging / f"{name}.partial", "shard-00000000"
        )
        for record in values:
            writer.add(record)
        row, outcomes = sharding._publish_generated_shard(
            writer, output, staging, "record-unsupported", 4096
        )
        require(row["contract_count"] == 2, "writer count differs")
        require(set(outcomes) == {"output_root", "staging"}, "sync labels differ")
        shard = output / "shard-00000000.inputs.jsonl.gz"
        outputs.append(shard.read_bytes())
        with gzip.open(shard, "rt", encoding="utf-8") as source:
            events = [json.loads(line) for line in source]
        require(len(events) == 6, "shared blobs were not deduplicated per shard")
    require(outputs[0] == outputs[1], "deterministic gzip bytes differ")

    capped = sharding.DeterministicShardWriter(
        root / "capped.partial", "shard-00000000"
    )
    capped.count = campaign.MAX_SHARD_CONTRACTS
    expect_failure(lambda: capped.add(values[0]), "shard cap was not enforced")
    capped.abort()

    byte_capped = sharding.DeterministicShardWriter(
        root / "byte-capped.partial",
        "shard-00000000",
        maximum_bytes=32,
    )
    expect_failure(
        lambda: (byte_capped.add(values[0]), byte_capped.finish()),
        "shard compressed-byte cap was not enforced",
    )
    byte_capped.abort()

    scratch_path = root / "scratch-capped.jsonl.gz"
    scratch_budget = campaign._ScratchBudget(16, None)
    expect_failure(
        lambda: campaign._write_checkpoint_merge_run(
            scratch_path,
            [
                {
                    "sort_key": "00" * 8,
                    "aggregate_id": "aggregate",
                    "raw_contract_id": "raw-contract-" + "0" * 64,
                }
            ],
            None,
            scratch_budget,
        ),
        "checkpoint merge scratch-byte cap was not enforced",
    )
    if scratch_path.exists():
        scratch_path.unlink()

    mixed = root / "mixed"
    mixed_stage = mixed / ".staging"
    mixed_stage.mkdir(parents=True)
    writer = sharding.DeterministicShardWriter(
        mixed_stage / "mixed.partial", "shard-00000000"
    )
    writer.add(values[0])
    expect_failure(
        lambda: sharding._publish_generated_shard(
            writer, mixed, mixed_stage, "record-unsupported", 16384
        ),
        "mixed-page final shard was accepted",
    )


def durability_fixture(root: Path) -> None:
    output = root / "durability"
    staging = output / ".staging"
    staging.mkdir(parents=True)
    stale = staging / (
        "shard-00000000.0123456789abcdef0123456789abcdef."
        "inputs.jsonl.gz.partial"
    )
    stale.write_bytes(b"stale")
    events: list[str] = []
    original_directory_sync = campaign._fsync_directory
    original_fsync = sharding.os.fsync
    try:
        campaign._fsync_directory = lambda path, _policy: (
            events.append(f"dir:{Path(path).name}") or "supported"
        )
        _staging, outcomes = sharding._prepare_root(output, "require")
        require(not stale.exists(), "stale stage was not removed")
        require(
            events[:2] == ["dir:.staging", "dir:durability"],
            "staging/root cleanup sync order differs",
        )
        require(outcomes == {"staging": "supported", "output_root": "supported"},
                "prepared sync outcomes differ")
        metadata = output / "existing.json"
        metadata.write_bytes(b"stable\n")
        events.clear()
        sharding.os.fsync = lambda _descriptor: events.append("file")
        sharding._write_or_compare(metadata, b"stable\n", "require")
        require(events == ["file", "dir:durability"],
                "existing metadata was not fsynced before its directory")
    finally:
        campaign._fsync_directory = original_directory_sync
        sharding.os.fsync = original_fsync


def bounded_scale_fixture(root: Path) -> None:
    merge_paths = [Path(f"producer-{index:03d}") for index in range(64)]
    rows_per_producer = 4096
    original_checkpoint_iterator = sharding.campaign._iter_frozen_checkpoint_contributions
    live_streams = 0
    maximum_live_streams = 0
    scratch_peak = 0

    def measure_scratch(_count: int) -> None:
        nonlocal scratch_peak
        size = sum(
            path.stat().st_size
            for directory in root.glob("a64-authority-merge-*")
            for path in directory.iterdir()
            if path.is_file()
        )
        scratch_peak = max(scratch_peak, size)

    def synthetic_checkpoint(path: Path):
        nonlocal live_streams, maximum_live_streams
        producer = int(path.name.split("-")[1])
        live_streams += 1
        maximum_live_streams = max(maximum_live_streams, live_streams)
        try:
            for row_index in range(rows_per_producer):
                ordinal = row_index * len(merge_paths) + producer
                yield {
                    "sort_key": ordinal.to_bytes(8, "big").hex(),
                    "aggregate_id": "aggregate-scale",
                    "raw_contract_id": f"raw-contract-{ordinal:064x}",
                }
        finally:
            live_streams -= 1

    try:
        sharding.campaign._iter_frozen_checkpoint_contributions = synthetic_checkpoint
        tracemalloc.start()
        count = 0
        previous = -1
        for row in campaign.merge_frozen_checkpoint_contributions(
            merge_paths,
            scratch_root=root,
            progress_callback=measure_scratch,
            fan_in=8,
        ):
            ordinal = int(str(row["sort_key"]), 16)
            require(ordinal > previous, "checkpoint merge order differs")
            previous = ordinal
            count += 1
        _current, merge_peak = tracemalloc.get_traced_memory()
        tracemalloc.stop()
    finally:
        sharding.campaign._iter_frozen_checkpoint_contributions = original_checkpoint_iterator
    require(count == len(merge_paths) * rows_per_producer,
            "scaled checkpoint merge count differs")
    require(maximum_live_streams <= 8,
            "checkpoint merge exceeded its bounded fan-in")
    require(merge_peak < 32 * 1024 * 1024,
            "checkpoint merge retained contribution-cardinality state")

    database_path = root / "scale.sqlite3"
    database = sharding._database(database_path)
    temporary_directory = database.execute(
        "PRAGMA temp_store_directory"
    ).fetchone()
    require(
        temporary_directory is not None
        and Path(str(temporary_directory[0])).resolve() == root.resolve(),
        "SQLite temporary storage escaped the checked sharding volume",
    )
    database.execute(
        "CREATE TABLE authoritative_contract_id("
        "identifier TEXT PRIMARY KEY) WITHOUT ROWID"
    )
    distinct = 1024
    for index in range(distinct):
        database.execute(
            "INSERT INTO authoritative_contract_id VALUES (?)",
            (f"input-contract-{index:064x}",),
        )
        database.execute(
            "INSERT INTO contract_id VALUES (?, ?, ?)",
            (
                f"input-contract-{index:064x}",
                "shard-00000000",
                f"raw-contract-{index:064x}",
            ),
        )
    campaign.require_exact_authoritative_id_set(database)
    source_paths = [Path(f"source-{index:05d}") for index in range(32768)]
    maximum_batch = 0
    original_materializations = sharding.iter_raw_materializations
    original_validate = sharding._validated_record

    def synthetic_materializations(paths):
        nonlocal maximum_batch
        batch = list(paths)
        maximum_batch = max(maximum_batch, len(batch))
        yield {"ordinal": int(batch[0].name.split("-")[1])}

    def synthetic_validate(record, _page):
        ordinal = int(record["ordinal"])
        identifier = f"input-contract-{ordinal % distinct:064x}"
        return identifier, f"raw-contract-{ordinal % distinct:064x}"

    try:
        sharding.iter_raw_materializations = synthetic_materializations
        sharding._validated_record = synthetic_validate
        observed = list(
            sharding._iter_distinct_source_materializations(
                source_paths, database, 4096
            )
        )
    finally:
        sharding.iter_raw_materializations = original_materializations
        sharding._validated_record = original_validate
        database.close()
    require(maximum_batch == 1, "full source corpus was reconstructed at once")
    require(len(observed) == distinct, "disk-backed global dedup differs")
    print(
        "A64 shard scale: "
        f"merged={count} merge_peak_bytes={merge_peak} "
        f"scratch_peak_bytes={scratch_peak} "
        f"dedup_sources={len(source_paths)} "
        f"dedup_db_bytes={database_path.stat().st_size} "
        f"process_peak_rss_bytes={resource.getrusage(resource.RUSAGE_SELF).ru_maxrss}"
    )


def end_to_end_fixture(root: Path) -> None:
    values = records()
    evidence = root / "evidence"
    evidence.mkdir()
    source = evidence / "producer.tsv.inputs.jsonl.gz"
    write_source(source, values)
    manifest = root / "contracts.tsv"
    write_contract_manifest(manifest, source)
    frozen = root / "frozen.tsv"
    frozen.write_text("frozen\n", encoding="utf-8")
    options = fixture_options(root, source, manifest, frozen)
    original_loader = sharding._load_authority

    def loader(_options, database, *_roots):
        return fake_authority(database, values, frozen, manifest)

    try:
        sharding._load_authority = loader
        result = sharding.run(options)
        output = options.output_directory
        shard = output / "shard-00000000.inputs.jsonl.gz"
        frozen_bytes = {
            path.name: path.read_bytes()
            for path in (
                shard,
                output / "shard-input-list.txt",
                output / "shard-set.json",
            )
        }
        require(result["total_contracts"] == len(values), "shard total differs")

        (output / "shard-set.json").unlink()
        resumed = sharding.run(options)
        require(resumed == result, "metadata-partial resume differs")
        require(
            all((output / name).read_bytes() == payload
                for name, payload in frozen_bytes.items()),
            "resume changed published bytes",
        )
        verified_resume = sharding.run(options)
        require(verified_resume == result, "fresh-DB published resume differs")
        require(
            all((output / name).read_bytes() == payload
                for name, payload in frozen_bytes.items()),
            "fresh-DB resume changed published bytes",
        )

        wrong_total = copy.copy(options)
        wrong_total.expected_total_contracts = len(values) + 1
        expect_failure(
            lambda: sharding.run(wrong_total),
            "resume ignored explicit expected total",
        )

        parsed = campaign.read_canonical_json(output / "shard-set.json")
        database = sharding._database(root / "strict.sqlite3")
        authority = fake_authority(database, values, frozen, manifest)
        try:
            mutated = copy.deepcopy(parsed)
            mutated["guest_page_size"] = 4096.0
            expect_failure(
                lambda: sharding._verify_shard_set(
                    output, mutated, authority, result["source_inputs"], database
                ),
                "float page size was accepted",
            )
            mutated = copy.deepcopy(parsed)
            mutated["shards"][0]["size"] = float(mutated["shards"][0]["size"])
            expect_failure(
                lambda: sharding._verify_shard_set(
                    output, mutated, authority, result["source_inputs"], database
                ),
                "float shard size was accepted",
            )
            mutated = copy.deepcopy(parsed)
            mutated["authoritative_input"]["input_contract_count"] = float(
                len(values)
            )
            expect_failure(
                lambda: sharding._verify_shard_set(
                    output, mutated, authority, result["source_inputs"], database
                ),
                "nested authority type substitution was accepted",
            )
            query_plan = " ".join(
                str(row)
                for row in database.execute(
                    "EXPLAIN QUERY PLAN SELECT MIN(raw_id), MAX(raw_id) "
                    "FROM authoritative_binding WHERE input_id = ?",
                    (str(values[0]["input_contract_id"]),),
                )
            )
            require(
                "authoritative_binding_by_input" in query_plan,
                "authority lookup does not use the input-leading index",
            )
        finally:
            database.close()

        unsafe = copy.copy(options)
        unsafe.output_directory = root / "unsafe\npath"
        expect_failure(
            lambda: sharding.run(unsafe),
            "line-ambiguous output path was accepted",
        )

        loader_calls = 0

        def counting_loader(*_arguments, **_keywords):
            nonlocal loader_calls
            loader_calls += 1
            raise AssertionError("authority loader reached after unsafe preflight")

        sharding._load_authority = counting_loader
        unsafe = copy.copy(options)
        unsafe.output_directory = evidence / "nested-output"
        expect_failure(
            lambda: sharding.run(unsafe),
            "shard output inside an evidence tree was accepted",
        )
        require(loader_calls == 0, "unsafe output reached authority loading")
        require(
            not unsafe.output_directory.exists(),
            "unsafe output preflight mutated the evidence tree",
        )

        unsafe = copy.copy(options)
        unsafe.local_temp_root = evidence
        expect_failure(
            lambda: sharding.run(unsafe),
            "shard local root inside an evidence tree was accepted",
        )
        require(loader_calls == 0, "unsafe local root reached authority loading")

        too_small = copy.copy(options)
        too_small.maximum_shard_bytes = shard.stat().st_size - 1
        expect_failure(
            lambda: sharding.run(too_small),
            "resume accepted a published shard above its current hard cap",
        )
        metadata_bytes = sum(
            (output / name).stat().st_size
            for name in sharding.SHARD_METADATA_NAMES
            if (output / name).exists()
        )
        too_small = copy.copy(options)
        too_small.maximum_metadata_bytes = metadata_bytes - 1
        expect_failure(
            lambda: sharding.run(too_small),
            "resume accepted metadata above its current hard cap",
        )
        require(loader_calls == 0, "resume hard-cap rejection loaded authority")

        unsafe = copy.copy(options)
        unsafe.output_directory = root / "wrong-layout"
        unsafe.output_directory.mkdir()
        (unsafe.output_directory / "shard-set.json").mkdir()
        expect_failure(
            lambda: sharding.run(unsafe),
            "reserved shard metadata directory was accepted",
        )
        require(loader_calls == 0, "unsafe layout reached authority loading")
        require(
            not (unsafe.output_directory / "campaign.lock").exists(),
            "unsafe layout created a campaign lock before rejection",
        )
    finally:
        sharding._load_authority = original_loader


def evidence_lock_fixture(root: Path) -> None:
    values = records()
    evidence = root / "lock-evidence"
    evidence.mkdir()
    source = evidence / "producer.tsv.inputs.jsonl.gz"
    write_source(source, values)
    manifest = root / "lock-contracts.tsv"
    write_contract_manifest(manifest, source)
    frozen = root / "lock-frozen.tsv"
    frozen.write_text("frozen\n", encoding="utf-8")
    options = fixture_options(
        root, source, manifest, frozen, output_name="lock-shards"
    )
    original_loader = sharding._load_authority
    loader_entered = threading.Event()
    started = threading.Event()
    result: list[object] = []
    failure: list[BaseException] = []

    def loader(_options, database, *_roots):
        loader_entered.set()
        return fake_authority(database, values, frozen, manifest)

    def execute() -> None:
        started.set()
        try:
            result.append(sharding.run(options))
        except BaseException as error:
            failure.append(error)

    lock_path = evidence_quiescence_lock_path(evidence)
    descriptor = os.open(lock_path, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_SH)
        sharding._load_authority = loader
        worker = threading.Thread(target=execute, daemon=True)
        worker.start()
        require(started.wait(1), "contended sharder thread did not start")
        time.sleep(0.1)
        require(worker.is_alive(), "sharder bypassed the live evidence lock")
        require(
            not loader_entered.is_set(),
            "authority snapshot began while a producer lock was held",
        )
        require(
            not options.output_directory.exists(),
            "sharder mutated output before acquiring its evidence snapshot",
        )
        fcntl.flock(descriptor, fcntl.LOCK_UN)
        worker.join(20)
        require(not worker.is_alive(), "sharder did not resume after lock release")
        if failure:
            raise failure[0]
        require(
            len(result) == 1 and loader_entered.is_set(),
            "sharder did not complete its locked snapshot",
        )
    finally:
        sharding._load_authority = original_loader
        try:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        finally:
            os.close(descriptor)


def authority_uniqueness_fixture() -> None:
    database = sqlite3.connect(":memory:")
    database.execute(
        "CREATE TABLE authoritative_binding("
        "aggregate_id TEXT, input_id TEXT, raw_id TEXT)"
    )
    database.executemany(
        "INSERT INTO authoritative_binding VALUES (?, ?, ?)",
        (
            ("aggregate-a", "input-a", "raw-a"),
            ("aggregate-b", "input-a", "raw-b"),
        ),
    )
    expect_failure(
        lambda: campaign.require_unique_authoritative_raw_binding(database),
        "multiple raw bindings for one canonical input were accepted",
    )
    database.close()


def capacity_boundary_fixture(root: Path) -> None:
    output = root / "capacity-output"
    local = root / "capacity-local"
    work = local / "work"
    staging = output / ".staging"
    work.mkdir(parents=True)
    staging.mkdir(parents=True)
    (work / "authority.sqlite3").write_bytes(b"d" * 100)
    (staging / "active.partial").write_bytes(b"s" * 50)
    options = argparse.Namespace(
        minimum_output_free_gib=1.5,
        minimum_local_free_gib=2.5,
        maximum_shard_bytes=1000,
        maximum_metadata_bytes=200,
        maximum_database_bytes=4096,
        maximum_merge_scratch_bytes=500,
    )
    output_required = int(1.5 * 1024**3) + 200 + 1000 - 50
    local_required = int(2.5 * 1024**3) + 4096 + 500 - 100
    free_by_path = {output: output_required, local: local_required}
    original_disk_usage = campaign.shutil.disk_usage

    def disk_usage(path):
        return type("DiskUsage", (), {"free": free_by_path[Path(path)]})()

    try:
        campaign.shutil.disk_usage = disk_usage
        sharding._require_sharding_capacity(
            options, output, local, work, staging
        )
        free_by_path[output] -= 1
        expect_failure(
            lambda: sharding._require_sharding_capacity(
                options, output, local, work, staging
            ),
            "sharding output reserve/headroom accepted one byte too little",
        )
        free_by_path[output] += 1
        free_by_path[local] -= 1
        expect_failure(
            lambda: sharding._require_sharding_capacity(
                options, output, local, work, staging
            ),
            "sharding local reserve/headroom accepted one byte too little",
        )
    finally:
        campaign.shutil.disk_usage = original_disk_usage


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="a64-campaign-sharding-test-") as temporary:
        root = Path(temporary).resolve()
        deterministic_writer_fixture(root)
        durability_fixture(root)
        bounded_scale_fixture(root)
        capacity_boundary_fixture(root)
        end_to_end_fixture(root)
        evidence_lock_fixture(root)
    authority_uniqueness_fixture()
    print("A64 ASLRef campaign sharding fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
