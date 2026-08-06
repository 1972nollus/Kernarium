#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Promote two verified page-profile ASLRef campaigns by exact frozen join."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import fcntl
import hashlib
import io
import os
import sqlite3
import tempfile
from contextlib import ExitStack, contextmanager
from pathlib import Path
from typing import Callable, Iterator, Mapping, Sequence

from a64_aggregate_manifest_identity import validate_manifest_identity
from aggregate_a64_raw_instruction_evidence import OUTPUT_COLUMNS as AGGREGATE_COLUMNS
from join_a64_input_materializations import INDEX_COLUMNS
import run_a64_aslref_campaign as campaign


PROMOTION_SCHEMA = "uml.a64.aslref-campaign-promotion/v1"
IDENTITY_SCHEMA = "uml.a64.aslref-campaign-promotion-identity/v2"
EXPECTED_ALLOCATED_VALID_AGGREGATES = 17_283
DEFAULT_MAX_PROMOTION_DATABASE_BYTES = 128 * 1024 * 1024 * 1024
PROMOTION_CAPACITY_INPUTS = 40_000_000
PROMOTION_CONSERVATIVE_BYTES_PER_INPUT = 1536
PROMOTION_CAPACITY_SAFETY_NUMERATOR = 3
PROMOTION_CAPACITY_SAFETY_DENOMINATOR = 2
PROMOTION_CAPACITY_REQUIRED_BYTES = (
    PROMOTION_CAPACITY_INPUTS
    * PROMOTION_CONSERVATIVE_BYTES_PER_INPUT
    * PROMOTION_CAPACITY_SAFETY_NUMERATOR
    // PROMOTION_CAPACITY_SAFETY_DENOMINATOR
)
PROMOTION_COLUMNS = [
    "schema",
    "aggregate_id",
    "execution_obligation_id",
    "allocation_status_id",
    "semantic_input_id",
    "guest_page_size",
    "input_contract_count",
    "raw_contract_id_count",
    "aggregate_input_raw_binding_sha256",
    "official_asl_shard_count",
    "official_asl_shard_ids_sha256",
    "official_asl_result_binding_sha256",
    "campaign_plan_sha256",
    "campaign_report_sha256",
    "join_model_digest",
    "engine_execution_evidence",
    "official_asl_execution",
    "official_semantic_status",
    "allocation_behavior_complete",
    "unresolved_requirements",
]


def require_promotion_capacity(
    local_temp_root: Path,
    work_root: Path | None,
    minimum_local_free_gib: float,
    maximum_database_bytes: int,
    maximum_merge_scratch_bytes: int,
) -> None:
    campaign._strict_integer(
        maximum_database_bytes, "promotion database cap", 4096
    )
    campaign._strict_integer(
        maximum_merge_scratch_bytes, "promotion merge scratch cap", 1
    )
    used = (
        0
        if work_root is None
        else sum(
            path.stat().st_size
            for path in work_root.rglob("*")
            if path.is_file()
        )
    )
    maximum = maximum_database_bytes + maximum_merge_scratch_bytes
    if used > maximum:
        raise ValueError("promotion local work exceeded its hard cap")
    campaign.require_free_space(
        local_temp_root,
        minimum_local_free_gib,
        "promotion local",
        headroom_bytes=maximum - used,
    )


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--campaign-4k-plan", type=Path, required=True)
    parser.add_argument("--campaign-4k-root", type=Path, required=True)
    parser.add_argument("--campaign-16k-plan", type=Path, required=True)
    parser.add_argument("--campaign-16k-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--identity-output", type=Path, required=True)
    parser.add_argument("--local-temp-root", type=Path, required=True)
    parser.add_argument(
        "--minimum-local-free-gib", type=float, default=20.0
    )
    parser.add_argument(
        "--maximum-database-bytes",
        type=int,
        default=DEFAULT_MAX_PROMOTION_DATABASE_BYTES,
    )
    parser.add_argument(
        "--maximum-merge-scratch-bytes",
        type=int,
        default=campaign.DEFAULT_MAX_AUTHORITY_MERGE_SCRATCH_BYTES,
    )
    parser.add_argument("--verify-only", action="store_true")
    return parser.parse_args()


def _iter_result_rows(
    root: Path,
    plan: Mapping[str, object],
) -> Iterator[dict[str, str]]:
    shards = plan["shards"]
    assert isinstance(shards, list)
    for shard in shards:
        shard_id = str(shard["shard_id"])
        directory = root / "shards" / shard_id
        success = campaign.read_canonical_json(directory / "SUCCESS.json")
        report = campaign.read_canonical_json(directory / "batch-report.json")
        workflow = report["campaign_workflow"]
        if not isinstance(workflow, dict):
            raise ValueError(f"{shard_id}: campaign workflow is absent")
        input_results = workflow["input_results"]
        if not isinstance(input_results, list):
            raise ValueError(f"{shard_id}: per-input results are absent")
        for result in input_results:
            if (
                not isinstance(result, dict)
                or set(result) != {
                    "input_contract_id", "official_asl_result"
                }
                or result["official_asl_result"] != "official-asl-passed"
            ):
                raise ValueError(f"{shard_id}: per-input result differs")
            yield {
                "input_id": str(result["input_contract_id"]),
                "shard_id": shard_id,
                "probe_report_sha256": str(success["probe_report_sha256"]),
                "probe_result_sha256": str(success["probe_result_sha256"]),
                "input_results_sha256": str(success["input_results_sha256"]),
                "artifact_manifest_sha256": str(
                    success["artifact_manifest_sha256"]
                ),
            }


def _add_digest_row(digest: object, row: Sequence[str]) -> None:
    payload = campaign.canonical_json(list(row)).encode("utf-8")
    digest.update(len(payload).to_bytes(8, "big"))  # type: ignore[attr-defined]
    digest.update(payload)  # type: ignore[attr-defined]


def install_official_results(
    database: sqlite3.Connection,
    rows: Iterator[Mapping[str, str]],
    progress_callback: Callable[[int], None] | None = None,
) -> int:
    database.execute(
        "CREATE TABLE verified_shard("
        "shard_id TEXT PRIMARY KEY, probe_report_sha256 TEXT NOT NULL, "
        "probe_result_sha256 TEXT NOT NULL, "
        "input_results_sha256 TEXT NOT NULL, "
        "artifact_manifest_sha256 TEXT NOT NULL) WITHOUT ROWID"
    )
    database.execute(
        "CREATE TABLE official_result("
        "input_id TEXT PRIMARY KEY, shard_id TEXT NOT NULL) WITHOUT ROWID"
    )
    count = 0
    current_shard: tuple[str, ...] | None = None
    try:
        for row in rows:
            if set(row) != {
                "input_id",
                "shard_id",
                "probe_report_sha256",
                "probe_result_sha256",
                "input_results_sha256",
                "artifact_manifest_sha256",
            }:
                raise ValueError("official ASL input-result row differs")
            if campaign.INPUT_ID.fullmatch(str(row["input_id"])) is None:
                raise ValueError("official ASL result input identity differs")
            if campaign.SHARD_ID.fullmatch(str(row["shard_id"])) is None or any(
                campaign.SHA256.fullmatch(str(row[name])) is None
                for name in (
                    "probe_report_sha256",
                    "probe_result_sha256",
                    "input_results_sha256",
                    "artifact_manifest_sha256",
                )
            ):
                raise ValueError("verified shard result identity differs")
            shard = tuple(str(row[name]) for name in (
                "shard_id",
                "probe_report_sha256",
                "probe_result_sha256",
                "input_results_sha256",
                "artifact_manifest_sha256",
            ))
            if current_shard is None or shard[0] != current_shard[0]:
                database.execute(
                    "INSERT INTO verified_shard VALUES (?, ?, ?, ?, ?)",
                    shard,
                )
                current_shard = shard
            elif shard != current_shard:
                raise ValueError("verified shard result identity differs")
            database.execute(
                "INSERT INTO official_result VALUES (?, ?)",
                (str(row["input_id"]), shard[0]),
            )
            count += 1
            if progress_callback is not None and count % 1024 == 0:
                progress_callback(count)
    except sqlite3.IntegrityError as error:
        raise ValueError("verified campaign per-input results repeat") from error
    return count


PROMOTION_BINDING_QUERY = (
    "SELECT b.aggregate_id, b.input_id, b.raw_id, r.shard_id, "
    "s.probe_report_sha256, s.probe_result_sha256, "
    "s.input_results_sha256, s.artifact_manifest_sha256 "
    "FROM authoritative_binding AS b "
    "JOIN official_result AS r ON r.input_id = b.input_id "
    "JOIN verified_shard AS s ON s.shard_id = r.shard_id "
    "ORDER BY b.aggregate_id, b.input_id"
)

PROMOTION_UNRELATED_RESULT_QUERY = (
    "SELECT r.input_id FROM official_result AS r "
    "LEFT JOIN authoritative_contract_id AS a ON a.identifier = r.input_id "
    "WHERE a.identifier IS NULL LIMIT 1"
)
PROMOTION_MISSING_RESULT_QUERY = (
    "SELECT a.identifier FROM authoritative_contract_id AS a "
    "LEFT JOIN official_result AS r ON r.input_id = a.identifier "
    "WHERE r.input_id IS NULL LIMIT 1"
)


def require_promotion_query_plans(database: sqlite3.Connection) -> None:
    input_plan = " ".join(
        str(row)
        for row in database.execute(
            "EXPLAIN QUERY PLAN SELECT raw_id FROM authoritative_binding "
            "INDEXED BY authoritative_binding_by_input WHERE input_id = ?",
            ("input-contract-" + "0" * 64,),
        )
    )
    if "authoritative_binding_by_input" not in input_plan:
        raise ValueError("promotion authority lookup lacks its input-leading index")
    join_plan = " ".join(
        str(row)
        for row in database.execute(
            "EXPLAIN QUERY PLAN " + PROMOTION_BINDING_QUERY
        )
    )
    if (
        "SCAN b USING" not in join_plan
        or "INDEX" not in join_plan
        or "SEARCH r USING" not in join_plan
        or "SEARCH s USING" not in join_plan
        or not any(token in join_plan for token in ("PRIMARY KEY", "INDEX"))
        or "AUTOMATIC" in join_plan
        or "USE TEMP B-TREE" in join_plan
    ):
        raise ValueError("promotion binding join lacks its bounded indexed plan")


def summarize_promotion_bindings(
    database: sqlite3.Connection,
    result_count: int,
) -> dict[str, dict[str, object]]:
    campaign._strict_integer(result_count, "official ASL result count", 1)
    observed_count = int(
        database.execute("SELECT COUNT(*) FROM official_result").fetchone()[0]
    )
    if observed_count != result_count:
        raise ValueError("official ASL result count differs")
    for query, searched_alias, label in (
        (
            PROMOTION_UNRELATED_RESULT_QUERY,
            "a",
            "official ASL result set has unrelated input identities",
        ),
        (
            PROMOTION_MISSING_RESULT_QUERY,
            "r",
            "official ASL result set is incomplete",
        ),
    ):
        campaign._require_indexed_anti_join(
            database, query, searched_alias, label
        )
        if database.execute(query).fetchone() is not None:
            raise ValueError(label)
    require_promotion_query_plans(database)

    summaries: dict[str, dict[str, object]] = {}
    aggregate_id: str | None = None
    binding_digest = hashlib.sha256()
    result_digest = hashlib.sha256()
    binding_count = 0
    shard_ids: set[str] = set()

    def finish_aggregate() -> None:
        nonlocal aggregate_id, binding_digest, result_digest, binding_count
        nonlocal shard_ids
        if aggregate_id is None:
            return
        shard_count, shard_digest = campaign._digest_tuples(
            (shard_id,) for shard_id in sorted(shard_ids)
        )
        summaries[aggregate_id] = {
            "binding_count": binding_count,
            "binding_digest": binding_digest.hexdigest(),
            "result_digest": result_digest.hexdigest(),
            "shard_count": shard_count,
            "shard_digest": shard_digest,
        }
        binding_digest = hashlib.sha256()
        result_digest = hashlib.sha256()
        binding_count = 0
        shard_ids = set()

    for row in database.execute(PROMOTION_BINDING_QUERY):
        values = tuple(str(value) for value in row)
        current = values[0]
        if current != aggregate_id:
            finish_aggregate()
            aggregate_id = current
        (
            _aggregate_id,
            input_id,
            raw_id,
            shard_id,
            probe_report_sha256,
            probe_result_sha256,
            input_results_sha256,
            artifact_manifest_sha256,
        ) = values
        _add_digest_row(binding_digest, (input_id, raw_id))
        _add_digest_row(
            result_digest,
            (
                input_id,
                raw_id,
                shard_id,
                probe_report_sha256,
                probe_result_sha256,
                input_results_sha256,
                artifact_manifest_sha256,
            ),
        )
        binding_count += 1
        shard_ids.add(shard_id)
    finish_aggregate()
    if not summaries:
        raise ValueError("promotion binding join is empty")
    return summaries


def collect_campaign(
    plan_path: Path,
    root: Path,
    *,
    lock_held: bool = False,
    local_temp_root: Path | None = None,
    minimum_local_free_gib: float = 0.0,
    maximum_database_bytes: int = DEFAULT_MAX_PROMOTION_DATABASE_BYTES,
    maximum_merge_scratch_bytes: int = (
        campaign.DEFAULT_MAX_AUTHORITY_MERGE_SCRATCH_BYTES
    ),
) -> dict[str, object]:
    if not lock_held:
        raise ValueError("campaign collection requires its output-root lock")
    plan_path = campaign.require_regular_file(plan_path, "campaign plan")
    root = campaign.require_root(root, "campaign output root")
    plan_bytes = plan_path.read_bytes()
    plan = campaign.validate_plan(
        campaign.parse_canonical_json(plan_bytes, str(plan_path))
    )
    if local_temp_root is not None:
        campaign.validate_run_root_isolation(
            plan_path, plan, root, local_temp_root
        )
        require_promotion_capacity(
            local_temp_root,
            None,
            minimum_local_free_gib,
            maximum_database_bytes,
            maximum_merge_scratch_bytes,
        )
    report = campaign.verify_campaign_locked(
        plan_path,
        plan,
        root,
        plan_bytes,
        local_temp_root,
        minimum_local_free_gib,
        False,
    )
    report_path = root / "campaign-report.json"

    authoritative = plan["authoritative_input"]
    assert isinstance(authoritative, dict)
    with tempfile.TemporaryDirectory(
        prefix="a64-campaign-promotion-", dir=local_temp_root
    ) as temporary:
        temporary_root = Path(temporary)
        database_path = temporary_root / "authority.sqlite3"
        database = sqlite3.connect(database_path)
        database.execute("PRAGMA journal_mode=OFF")
        database.execute("PRAGMA synchronous=OFF")
        campaign.configure_bounded_sqlite_storage(
            database, database_path, maximum_database_bytes
        )
        database.execute(
            "CREATE TABLE contract_id("
            "identifier TEXT PRIMARY KEY, shard_id TEXT NOT NULL, "
            "raw_id TEXT NOT NULL)"
        )
        shards = plan["shards"]
        assert isinstance(shards, list)
        for shard in shards:
            source = shard["input"]
            assert isinstance(source, dict)
            count, digest = campaign.scan_shard_input(
                Path(str(source["path"])),
                database,
                str(shard["shard_id"]),
                int(plan["guest_page_size"]),
            )
            if (
                count != shard["contract_count"]
                or digest != shard["input_contract_ids_sha256"]
            ):
                raise ValueError("campaign input changed during promotion join")
        observed_authority = campaign.load_authoritative_input(
            guest_page_size=int(plan["guest_page_size"]),
            aggregate_manifest=Path(
                str(authoritative["aggregate_manifest"]["path"])
            ),
            aggregate_manifest_identity=Path(
                str(authoritative["aggregate_manifest_identity_file"]["path"])
            ),
            materialization_index=Path(
                str(authoritative["materialization_index"]["path"])
            ),
            contract_manifests=[
                Path(str(entry["path"]))
                for entry in authoritative["contract_manifests"]
            ],
            database=database,
            progress_callback=(
                None
                if local_temp_root is None
                else lambda _count: require_promotion_capacity(
                    local_temp_root,
                    temporary_root,
                    minimum_local_free_gib,
                    maximum_database_bytes,
                    maximum_merge_scratch_bytes,
                )
            ),
            maximum_merge_scratch_bytes=maximum_merge_scratch_bytes,
            before_merge_scratch_write=(
                None
                if local_temp_root is None
                else lambda _bytes: require_promotion_capacity(
                    local_temp_root,
                    temporary_root,
                    minimum_local_free_gib,
                    maximum_database_bytes,
                    maximum_merge_scratch_bytes,
                )
            ),
        )
        database.execute("DROP TABLE contract_id")
        result_count = install_official_results(
            database,
            _iter_result_rows(root, plan),
            (
                None
                if local_temp_root is None
                else lambda _count: require_promotion_capacity(
                    local_temp_root,
                    temporary_root,
                    minimum_local_free_gib,
                    maximum_database_bytes,
                    maximum_merge_scratch_bytes,
                )
            ),
        )
        aggregate_summaries = summarize_promotion_bindings(
            database, result_count
        )
        if local_temp_root is not None:
            require_promotion_capacity(
                local_temp_root,
                temporary_root,
                minimum_local_free_gib,
                maximum_database_bytes,
                maximum_merge_scratch_bytes,
            )
        database.close()
    if observed_authority != authoritative:
        raise ValueError("campaign authority changed during promotion join")
    campaign.require_plan_snapshot(plan_path, plan_bytes)

    index_rows = campaign.read_tsv(
        Path(str(authoritative["materialization_index"]["path"])),
        INDEX_COLUMNS,
    )
    index = {row["aggregate_id"]: row for row in index_rows}
    if len(index) != len(index_rows):
        raise ValueError("materialization index repeats aggregate identities")
    return {
        "page": int(plan["guest_page_size"]),
        "plan": plan,
        "plan_identity": campaign.file_identity(plan_path),
        "root": root,
        "report": report,
        "report_identity": campaign.file_identity(report_path),
        "result_count": result_count,
        "aggregate_summaries": aggregate_summaries,
        "index": index,
        "authority": authoritative,
    }


def build_promotions(
    aggregates: Sequence[Mapping[str, str]],
    campaigns: Sequence[Mapping[str, object]],
    *,
    expected_allocated_valid_count: int = EXPECTED_ALLOCATED_VALID_AGGREGATES,
) -> list[dict[str, str]]:
    pages = {int(record["page"]) for record in campaigns}
    if len(campaigns) != 2 or pages != {4096, 16384}:
        raise ValueError("promotion requires separate exact 4K and 16K campaigns")
    by_page = {int(record["page"]): record for record in campaigns}
    allocated = {
        row["aggregate_id"]: row
        for row in aggregates
        if row["validity_partition"] == "allocated-valid"
    }
    if len(allocated) != expected_allocated_valid_count:
        raise ValueError(
            "frozen allocated-valid aggregate count differs: "
            f"{len(allocated)} != {expected_allocated_valid_count}"
        )

    eligible: set[str] = set()
    output: list[dict[str, str]] = []
    for page in (4096, 16384):
        record = by_page[page]
        index = record["index"]
        summaries = record["aggregate_summaries"]
        if not isinstance(index, Mapping) or not isinstance(summaries, Mapping):
            raise ValueError("campaign promotion record fields differ")
        result_count = campaign._strict_integer(
            record["result_count"], "campaign promotion result count", 1
        )
        report = record["report"]
        if not isinstance(report, Mapping) or result_count != campaign._strict_integer(
            report["published_probe_result_binding_count"],
            "campaign report result binding count",
            1,
        ):
            raise ValueError("campaign promotion result count differs")
        page_allocated = {
            aggregate_id
            for aggregate_id, row in allocated.items()
            if int(row["guest_page_size"]) == page
        }
        page_index_eligible = {
            str(aggregate_id)
            for aggregate_id, row in index.items()
            if isinstance(row, Mapping)
            and row["validity_partition"] == "allocated-valid"
            and row["materialization_status"]
            == "materialized-execution-ready"
            and row["official_semantic_status"]
            == "pending-official-asl-execution"
            and row["representation_status"] == "representable"
            and row["representation_obligations"] == "none"
            and row["mapping_status"] == "exact-allocation"
            and row["allocation_behavior_complete"] == "0"
        }
        if page_index_eligible != page_allocated:
            raise ValueError(
                f"{page}: frozen execution-ready index coverage differs"
            )
        if set(summaries) != page_allocated:
            raise ValueError("allocated-valid aggregate binding coverage differs")

        for aggregate_id in sorted(page_allocated):
            summary = summaries[aggregate_id]
            if not isinstance(summary, Mapping) or set(summary) != {
                "binding_count",
                "binding_digest",
                "result_digest",
                "shard_count",
                "shard_digest",
            }:
                raise ValueError("campaign aggregate binding summary differs")
            binding_count = campaign._strict_integer(
                summary["binding_count"],
                "campaign aggregate binding count",
                1,
            )
            shard_count = campaign._strict_integer(
                summary["shard_count"],
                "campaign aggregate shard count",
                1,
            )
            for name in ("binding_digest", "result_digest", "shard_digest"):
                if (
                    not isinstance(summary[name], str)
                    or campaign.SHA256.fullmatch(str(summary[name])) is None
                ):
                    raise ValueError("campaign aggregate digest differs")
            index_row = index[aggregate_id]
            assert isinstance(index_row, Mapping)
            if (
                int(str(index_row["input_contract_count"])) != binding_count
                or int(str(index_row["raw_contract_id_count"])) != binding_count
            ):
                raise ValueError(
                    f"{aggregate_id}: authoritative binding count differs"
                )
            plan_identity = record["plan_identity"]
            report_identity = record["report_identity"]
            authority = record["authority"]
            assert isinstance(plan_identity, Mapping)
            assert isinstance(report_identity, Mapping)
            assert isinstance(authority, Mapping)
            output.append(
                {
                    "schema": PROMOTION_SCHEMA,
                    "aggregate_id": aggregate_id,
                    "execution_obligation_id": str(
                        index_row["execution_obligation_id"]
                    ),
                    "allocation_status_id": str(
                        index_row["allocation_status_id"]
                    ),
                    "semantic_input_id": str(index_row["semantic_input_id"]),
                    "guest_page_size": str(page),
                    "input_contract_count": str(binding_count),
                    "raw_contract_id_count": str(binding_count),
                    "aggregate_input_raw_binding_sha256": str(
                        summary["binding_digest"]
                    ),
                    "official_asl_shard_count": str(shard_count),
                    "official_asl_shard_ids_sha256": str(
                        summary["shard_digest"]
                    ),
                    "official_asl_result_binding_sha256": str(
                        summary["result_digest"]
                    ),
                    "campaign_plan_sha256": str(plan_identity["sha256"]),
                    "campaign_report_sha256": str(report_identity["sha256"]),
                    "join_model_digest": str(authority["join_model_digest"]),
                    "engine_execution_evidence": (
                        "interpreter+gadgets+jit-or-host-feature-skip"
                    ),
                    "official_asl_execution": "passed-exact-input-set",
                    "official_semantic_status": "official-asl-executed",
                    "allocation_behavior_complete": "1",
                    "unresolved_requirements": "none",
                }
            )
        eligible.update(page_allocated)
    if eligible != set(allocated) or len(output) != expected_allocated_valid_count:
        raise ValueError("combined campaign promotion coverage is not exact")
    output.sort(key=lambda row: row["aggregate_id"])
    return output


def tsv_payload(rows: Sequence[Mapping[str, str]]) -> bytes:
    output = io.StringIO(newline="")
    writer = csv.DictWriter(
        output,
        fieldnames=PROMOTION_COLUMNS,
        delimiter="\t",
        lineterminator="\n",
    )
    writer.writeheader()
    writer.writerows(rows)
    return output.getvalue().encode("utf-8")


def promotion_identity(
    rows: Sequence[Mapping[str, str]],
    payload: bytes,
    records: Sequence[Mapping[str, object]],
    frozen_identity: Mapping[str, object],
    join_model_digest: object,
    resource_caps: Mapping[str, int] | None = None,
) -> dict[str, object]:
    if resource_caps is None:
        resource_caps = {
            "maximum_database_bytes": (
                DEFAULT_MAX_PROMOTION_DATABASE_BYTES
            ),
            "maximum_merge_scratch_bytes": (
                campaign.DEFAULT_MAX_AUTHORITY_MERGE_SCRATCH_BYTES
            ),
        }
    if set(resource_caps) != {
        "maximum_database_bytes", "maximum_merge_scratch_bytes"
    }:
        raise ValueError("promotion resource-cap binding differs")
    for name, value in resource_caps.items():
        campaign._strict_integer(
            value,
            f"promotion {name}",
            4096 if name == "maximum_database_bytes" else 1,
        )
    return {
        "schema": IDENTITY_SCHEMA,
        "aggregate_manifest_identity": dict(frozen_identity),
        "join_model_digest": join_model_digest,
        "resource_caps": dict(resource_caps),
        "capacity_model": {
            "maximum_input_contracts": PROMOTION_CAPACITY_INPUTS,
            "conservative_bytes_per_input": (
                PROMOTION_CONSERVATIVE_BYTES_PER_INPUT
            ),
            "safety_factor_numerator": PROMOTION_CAPACITY_SAFETY_NUMERATOR,
            "safety_factor_denominator": PROMOTION_CAPACITY_SAFETY_DENOMINATOR,
            "required_database_bytes": PROMOTION_CAPACITY_REQUIRED_BYTES,
            "configured_database_cap_satisfies_model": (
                resource_caps["maximum_database_bytes"]
                >= PROMOTION_CAPACITY_REQUIRED_BYTES
            ),
        },
        "row_count": len(rows),
        "guest_page_size_counts": {
            "4096": sum(row["guest_page_size"] == "4096" for row in rows),
            "16384": sum(row["guest_page_size"] == "16384" for row in rows),
        },
        "columns_sha256": hashlib.sha256(
            ("\t".join(PROMOTION_COLUMNS) + "\n").encode("ascii")
        ).hexdigest(),
        "tsv_sha256": hashlib.sha256(payload).hexdigest(),
        "campaigns": [
            {
                "guest_page_size": record["page"],
                "campaign_plan": record["plan_identity"],
                "campaign_report": record["report_identity"],
                "published_probe_result_binding_count": record["report"][
                    "published_probe_result_binding_count"
                ],
                "published_probe_result_bindings_sha256": record["report"][
                    "published_probe_result_bindings_sha256"
                ],
            }
            for record in records
        ],
        "official_semantic_status": "official-asl-executed",
        "allocation_behavior_complete": True,
        "unresolved_requirements": "none",
    }


def canonical_output_target(path: Path, label: str) -> Path:
    return campaign.canonical_write_target(path, label)


@contextmanager
def promotion_output_lock(output: Path, identity_output: Path):
    canonical_output = canonical_output_target(output, "promotion output")
    canonical_identity = canonical_output_target(
        identity_output, "promotion identity output"
    )
    if not campaign.paths_alias(
        canonical_output.parent, canonical_identity.parent
    ):
        raise ValueError("promotion output pair must share one directory")
    lock_path = canonical_output.parent / ".a64-aslref-promotion.lock"
    if campaign.paths_alias(
        canonical_output, lock_path
    ) or campaign.paths_alias(canonical_identity, lock_path):
        raise ValueError("promotion output aliases its pair lock")
    if lock_path.is_symlink() or (
        lock_path.exists() and not lock_path.is_file()
    ):
        raise ValueError("promotion output-pair lock is unsafe")
    created = not lock_path.exists()
    descriptor = os.open(
        lock_path,
        os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0),
        0o600,
    )
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        if created:
            os.fsync(descriptor)
            campaign._fsync_directory(lock_path.parent, "require")
        yield
    finally:
        fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)


def validate_output_targets(
    output: Path,
    identity_output: Path,
    *,
    forbidden_directories: Sequence[Path] = (),
    forbidden_files: Sequence[Path] = (),
) -> None:
    canonical_output = canonical_output_target(output, "promotion output")
    canonical_identity = canonical_output_target(
        identity_output, "promotion identity output"
    )
    if not campaign.paths_alias(
        canonical_output.parent, canonical_identity.parent
    ):
        raise ValueError("promotion output pair must share one directory")
    if campaign._path_contains(
        canonical_output, canonical_identity
    ) or campaign._path_contains(canonical_identity, canonical_output):
        raise ValueError("promotion TSV and identity outputs alias one file")
    for target, label in (
        (canonical_output, "promotion TSV"),
        (canonical_identity, "promotion identity"),
    ):
        campaign.validate_write_target_isolation(
            target,
            forbidden_directories=forbidden_directories,
            forbidden_files=forbidden_files,
            label=label,
        )


def verify_promotion_outputs(
    output: Path,
    identity_output: Path,
    expected_rows: Sequence[Mapping[str, str]],
    expected_identity: Mapping[str, object],
) -> Mapping[str, object]:
    for path, label in (
        (output, "promotion TSV"),
        (identity_output, "promotion identity"),
    ):
        campaign.require_regular_file(path, label)
    payload = output.read_bytes()
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError("promotion TSV is not UTF-8") from error
    reader = csv.DictReader(io.StringIO(text, newline=""), delimiter="\t")
    if reader.fieldnames != PROMOTION_COLUMNS:
        raise ValueError("promotion TSV columns differ")
    observed_rows = list(reader)
    if not observed_rows or any(set(row) != set(PROMOTION_COLUMNS) for row in observed_rows):
        raise ValueError("promotion TSV rows differ")
    identifiers = [row["aggregate_id"] for row in observed_rows]
    if identifiers != sorted(set(identifiers)):
        raise ValueError("promotion aggregate IDs are not sorted unique")
    if tsv_payload(observed_rows) != payload:
        raise ValueError("promotion TSV is not canonical")
    if observed_rows != list(expected_rows):
        raise ValueError("promotion rows differ from verified campaign join")
    identity = campaign.read_canonical_json(identity_output)
    if identity != expected_identity:
        raise ValueError("promotion identity differs from verified campaign join")
    if identity["tsv_sha256"] != hashlib.sha256(payload).hexdigest():
        raise ValueError("promotion identity TSV digest differs")
    return identity


def publish_or_resume_promotion_outputs(
    output: Path,
    identity_output: Path,
    payload: bytes,
    identity: Mapping[str, object],
    expected_rows: Sequence[Mapping[str, str]],
    *,
    verify_only: bool,
) -> Mapping[str, object]:
    output_exists = output.exists()
    identity_exists = identity_output.exists()
    if output_exists and identity_exists:
        return verify_promotion_outputs(
            output, identity_output, expected_rows, identity
        )
    if verify_only:
        raise ValueError("promotion outputs do not exist for verification")
    identity_payload = (campaign.canonical_json(identity) + "\n").encode(
        "utf-8"
    )
    if output_exists:
        if campaign.require_regular_file(
            output, "partial promotion TSV"
        ).read_bytes() != payload:
            raise ValueError("partial promotion TSV differs from verified join")
        campaign.atomic_write(identity_output, identity_payload, "require")
    elif identity_exists:
        if campaign.read_canonical_json(identity_output) != identity:
            raise ValueError(
                "partial promotion identity differs from verified join"
            )
        identity_output.unlink()
        campaign._fsync_directory(identity_output.parent, "require")
        campaign.atomic_write(output, payload, "require")
        campaign.atomic_write(identity_output, identity_payload, "require")
    else:
        campaign.atomic_write(output, payload, "require")
        campaign.atomic_write(identity_output, identity_payload, "require")
    return verify_promotion_outputs(
        output, identity_output, expected_rows, identity
    )


def run(options: argparse.Namespace) -> Mapping[str, object]:
    validate_output_targets(options.output, options.identity_output)
    local_temp_root = campaign.require_root(
        options.local_temp_root, "promotion local temp root"
    )
    require_promotion_capacity(
        local_temp_root,
        None,
        options.minimum_local_free_gib,
        options.maximum_database_bytes,
        options.maximum_merge_scratch_bytes,
    )
    specifications = [
        (
            campaign.require_regular_file(
                options.campaign_4k_plan, "4K campaign plan"
            ),
            campaign.require_root(options.campaign_4k_root, "4K campaign root"),
        ),
        (
            campaign.require_regular_file(
                options.campaign_16k_plan, "16K campaign plan"
            ),
            campaign.require_root(options.campaign_16k_root, "16K campaign root"),
        ),
    ]
    roots = [root for _plan, root in specifications]
    if len(set(roots)) != 2:
        raise ValueError("4K and 16K campaigns require distinct output roots")
    with promotion_output_lock(options.output, options.identity_output):
        with ExitStack() as stack:
            for root in sorted(roots, key=lambda path: path.as_posix()):
                stack.enter_context(
                    campaign.campaign_lock(
                        root, "record-unsupported", create=False
                    )
                )
            records = [
                collect_campaign(
                    plan,
                    root,
                    lock_held=True,
                    local_temp_root=local_temp_root,
                    minimum_local_free_gib=options.minimum_local_free_gib,
                    maximum_database_bytes=options.maximum_database_bytes,
                    maximum_merge_scratch_bytes=(
                        options.maximum_merge_scratch_bytes
                    ),
                )
                for plan, root in specifications
            ]
            return finish_run(options, records)


def finish_run(
    options: argparse.Namespace, records: list[dict[str, object]]
) -> Mapping[str, object]:
    records.sort(key=lambda record: int(record["page"]))
    authorities = [record["authority"] for record in records]
    if any(not isinstance(authority, Mapping) for authority in authorities):
        raise ValueError("campaign authoritative binding differs")
    first = authorities[0]
    second = authorities[1]
    assert isinstance(first, Mapping) and isinstance(second, Mapping)
    if (
        first["aggregate_manifest_identity"]
        != second["aggregate_manifest_identity"]
        or first["aggregate_manifest"]["sha256"]  # type: ignore[index]
        != second["aggregate_manifest"]["sha256"]  # type: ignore[index]
        or first["join_model_digest"] != second["join_model_digest"]
    ):
        raise ValueError("4K/16K campaigns do not share one frozen join model")
    plans = [record["plan"] for record in records]
    if any(not isinstance(plan, Mapping) for plan in plans):
        raise ValueError("campaign plan snapshot differs")
    first_plan = plans[0]
    second_plan = plans[1]
    assert isinstance(first_plan, Mapping) and isinstance(second_plan, Mapping)
    if first_plan["probe"] != second_plan["probe"]:
        raise ValueError("4K/16K campaigns use different official probe tools")
    first_freeze = {
        str(entry["path"]): entry for entry in first_plan["source_freeze"]  # type: ignore[index]
    }
    second_freeze = {
        str(entry["path"]): entry for entry in second_plan["source_freeze"]  # type: ignore[index]
    }
    probe = first_plan["probe"]
    assert isinstance(probe, Mapping)
    for value in probe.values():
        if not isinstance(value, str):
            continue
        if value not in first_freeze or first_freeze[value] != second_freeze.get(value):
            raise ValueError(
                "4K/16K campaigns lack identical frozen official/probe input "
                f"{value}"
            )
    aggregate_path = Path(str(first["aggregate_manifest"]["path"]))  # type: ignore[index]
    aggregates = campaign.read_tsv(aggregate_path, AGGREGATE_COLUMNS)
    frozen_identity = first["aggregate_manifest_identity"]
    assert isinstance(frozen_identity, Mapping)
    validate_manifest_identity(aggregates, AGGREGATE_COLUMNS, frozen_identity)
    rows = build_promotions(aggregates, records)
    payload = tsv_payload(rows)
    identity = promotion_identity(
        rows,
        payload,
        records,
        frozen_identity,
        first["join_model_digest"],
        {
            "maximum_database_bytes": options.maximum_database_bytes,
            "maximum_merge_scratch_bytes": (
                options.maximum_merge_scratch_bytes
            ),
        },
    )
    protected_directories = [
        Path(str(record["root"])) for record in records
    ]
    protected_directories.append(
        campaign.require_root(
            options.local_temp_root, "promotion local temp root"
        )
    )
    protected_files: list[Path] = []
    for record in records:
        plan_identity = record["plan_identity"]
        plan = record["plan"]
        assert isinstance(plan_identity, Mapping)
        assert isinstance(plan, Mapping)
        protected_files.append(Path(str(plan_identity["path"])))
        for entry in plan["source_freeze"]:  # type: ignore[index]
            assert isinstance(entry, Mapping)
            path = Path(str(entry["path"]))
            if entry["kind"] in {"tree", "python-runtime"}:
                protected_directories.append(path)
            else:
                protected_files.append(path)
        for shard in plan["shards"]:  # type: ignore[index]
            assert isinstance(shard, Mapping)
            source = shard["input"]
            assert isinstance(source, Mapping)
            protected_files.append(Path(str(source["path"])))
    validate_output_targets(
        options.output,
        options.identity_output,
        forbidden_directories=protected_directories,
        forbidden_files=protected_files,
    )
    return publish_or_resume_promotion_outputs(
        options.output,
        options.identity_output,
        payload,
        identity,
        rows,
        verify_only=options.verify_only,
    )


def main() -> int:
    identity = run(arguments())
    print(campaign.canonical_json(identity))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, sqlite3.DatabaseError) as error:
        print(f"error: {error}", file=__import__("sys").stderr)
        raise SystemExit(1)
