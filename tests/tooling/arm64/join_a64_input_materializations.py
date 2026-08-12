#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Join concrete native inputs to every checked official A64 aggregate.

The native JSONL recorder intentionally emits no official stable ID.  This
tool independently repeats the checked Arm allocation/decode mapping used by
the aggregate generator, verifies each resulting aggregate identity and its
declared digest, and writes a compact index.  Full installed code pages are
validated one JSON object at a time and are never retained by this join.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import base64
import binascii
import csv
import gzip
import hashlib
import io
import json
import math
import os
import re
import shutil
import sqlite3
import sys
import tempfile
import zlib
from collections import defaultdict
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterable, Iterator, Mapping, TextIO

from a64_input_materialization import (
    AGGREGATE_SCHEMA,
    canonical_json,
    iter_raw_materializations,
    materialization_id,
    projection_inventory,
    read_callback_contract_registry,
    raw_contract_id,
    raw_contract_tsv,
    representation_obligations,
    validate_callback_contract_registry_use,
    validate_callback_registry_source_coverage,
)
from a64_raw_evidence_files import (
    atomic_publish_bytes,
    canonical_existing_directory,
    path_is_ancestor,
    paths_alias,
    require_no_symlink_components,
)
from aggregate_a64_raw_instruction_evidence import (
    AGGREGATE_ID_COLUMNS,
    OUTPUT_COLUMNS as AGGREGATE_COLUMNS,
    Pattern,
    aggregate_id,
    alternatives_text,
    fallback_features,
    fault_partition,
    parse_constraints,
    parse_patterns,
    read_tsv,
    select_allocations,
    select_decode_partition,
    semantic_features_for_word,
    source_identity,
)
from generate_a64_aslref_aggregate_execution_plan import build_plan
from generate_a64_instruction_allocations import exact_features
from generate_a64_instruction_decode_partitions import COLUMNS as DECODE_COLUMNS
from generate_a64_instruction_required_variants import COLUMNS as REQUIRED_COLUMNS
from validate_a64_instruction_aggregate_manifest import (
    validate_checked_manifest_rows,
)


INDEX_COLUMNS = [
    "schema",
    "aggregate_id",
    "execution_obligation_id",
    "allocation_status_id",
    "semantic_input_id",
    "decode_partition_id",
    "validity_partition",
    "materialization_status",
    "official_semantic_status",
    "input_contract_count",
    "raw_contract_id_count",
    "raw_contract_id_digest",
    "input_materialization_id_count",
    "input_materialization_id_digest",
    "representation_status",
    "representation_obligations",
    "declared_count",
    "declared_digest",
    "observed_digest",
    "mapping_status",
    "allocation_behavior_complete",
]

JOIN_MODEL_SCHEMA = "uml.a64.input-materialization-join-model/v4"
CONTRACT_INDEX_SCHEMA = "uml.a64.aggregate-input-contract-index/v4"
CONTRACT_INDEX_COLUMNS = [
    "schema",
    "aggregate_id",
    "raw_contract_id",
    "input_materialization_id",
    "raw_occurrences",
    "representation_obligations",
]

CONTRIBUTION_CHECKPOINT_HEADER_SCHEMA = (
    "uml.a64.input-contribution-checkpoint-header/v2"
)
CONTRIBUTION_CHECKPOINT_FOOTER_SCHEMA = (
    "uml.a64.input-contribution-checkpoint-footer/v2"
)
CONTRIBUTION_CHECKPOINT_COLUMNS = [
    "schema",
    "aggregate_id",
    "sort_key",
    "declaration_base64",
    "raw_contract_id",
    "input_materialization_id",
    "raw_occurrences",
    "representation_obligations",
]
CONTRACT_MANIFEST_SCHEMA = "uml.a64.aggregate-input-contract-manifest/v2"
CONTRACT_MANIFEST_COLUMNS = [
    "schema",
    "producer",
    "checkpoint_file",
    "checkpoint_sha256",
    "checkpoint_size",
    "source_name",
    "source_sha256",
    "source_size",
    "join_model_digest",
    "guest_page_size",
    "contribution_count",
    "contribution_digest",
]

ENGINE_ORDER = {
    "unavailable": 0,
    "skipped-host-feature": 1,
    "executed-not-retired": 2,
    "executed-retired": 3,
}
SCOPE_ORDER = {"instruction": 0, "program": 1}
SPOOL_METADATA_ALLOWANCE_BYTES = 4 * 1024 * 1024
CHECKPOINT_METADATA_ALLOWANCE_BYTES = 1024 * 1024
# The largest failed 16 KiB producer declared 157,080 contributions and reached
# 164,941,824 bytes before completion: at least 1,050.05 bytes per declared
# contribution including indexes.  It exhausted the former 1,024-byte
# allowance, then completed at 2,048 and advanced to aggregate validation.
# This direct successful replay keeps the bound finite without guessing from
# the incomplete failure-run row count.
DEFAULT_SPOOL_BYTES_PER_CONTRIBUTION = 2048
SQLITE_CAP_PROXIMITY_PAGES = 256


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--evidence-directory", action="append", type=Path, default=[]
    )
    parser.add_argument("--input-jsonl", action="append", type=Path, default=[])
    parser.add_argument(
        "--aggregate-manifest",
        type=Path,
        default=repository
        / "tests/unit/arm64/a64_instruction_aggregate_manifest.tsv",
    )
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
        "--projection-inventory",
        type=Path,
        default=repository
        / "tests/unit/arm64/a64_input_materialization_projection.json",
    )
    parser.add_argument(
        "--callback-contracts",
        type=Path,
        default=repository / "tests/unit/arm64/a64_callback_contracts.tsv",
    )
    parser.add_argument("--guest-page-size", type=int, choices=(4096, 16384))
    parser.add_argument("--allow-partial", action="store_true")
    parser.add_argument("--require-execution-ready", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--contract-index-output", type=Path)
    parser.add_argument("--checkpoint-directory", type=Path)
    parser.add_argument("--free-space-reserve-gib", type=float, default=2.0)
    parser.add_argument(
        "--spool-bytes-per-contribution",
        type=int,
        default=DEFAULT_SPOOL_BYTES_PER_CONTRIBUTION,
    )
    parser.add_argument(
        "--checkpoint-bytes-per-contribution", type=int, default=128
    )
    return parser.parse_args()


@dataclass(frozen=True)
class MappingContext:
    allocations: dict[int, list[Pattern]]
    hierarchy: dict[int, list[Pattern]]
    tables: dict[str, list[Pattern]]
    decode_id_by_identity: dict[tuple[str, ...], str]


@dataclass(frozen=True)
class MappedIdentity:
    aggregate_id: str
    fields: dict[str, str]


@dataclass(frozen=True)
class CheckpointInfo:
    path: Path
    producer: str
    source_name: str
    source_sha256: str
    source_size: int
    join_model_digest: str
    guest_page_size: int
    contribution_count: int
    contribution_digest: str
    checkpoint_sha256: str
    checkpoint_size: int


@dataclass
class AggregateAccumulator:
    contract_count: int = 0
    declared_count: int = 0
    declared_digest: Any = field(default_factory=hashlib.sha256)
    raw_id_digest: Any = field(default_factory=hashlib.sha256)
    input_id_digest: Any = field(default_factory=hashlib.sha256)
    obligations: set[str] = field(default_factory=set)
    pending_sort_key: str | None = None
    pending_declaration_prefix: bytes = b""
    pending_occurrences: int = 0
    finished: bool = False

    def _flush_declaration(self) -> None:
        if self.pending_sort_key is None:
            return
        self.declared_digest.update(
            self.pending_declaration_prefix
            + str(self.pending_occurrences).encode("ascii")
            + b"\n"
        )
        self.pending_sort_key = None
        self.pending_declaration_prefix = b""
        self.pending_occurrences = 0

    def finish(self) -> None:
        if not self.finished:
            self._flush_declaration()
            self.finished = True

    def add(self, row: Mapping[str, object]) -> None:
        if self.finished:
            raise ValueError("contribution added after aggregate finalization")
        try:
            declaration_bytes = base64.b64decode(
                str(row["declaration_base64"]), validate=True
            )
            occurrences = int(str(row["raw_occurrences"]))
            sort_key = str(row["sort_key"])
            raw_id = str(row["raw_contract_id"])
            input_id = str(row["input_materialization_id"])
            obligations = row["representation_obligations"]
        except (KeyError, ValueError, binascii.Error) as error:
            raise ValueError("invalid contribution checkpoint row") from error
        fields = declaration_bytes.removesuffix(b"\n").split(b"\0")
        if (
            not declaration_bytes.endswith(b"\n")
            or len(fields) != len(DECLARED_COLUMNS)
        ):
            raise ValueError("invalid contribution declaration framing")
        try:
            declared_occurrences = int(fields[-1].decode("ascii"))
        except (UnicodeDecodeError, ValueError) as error:
            raise ValueError("invalid contribution declaration count") from error
        declaration_prefix = b"\0".join(fields[:-1]) + b"\0"
        if (
            occurrences <= 0
            or declared_occurrences != occurrences
            or not sort_key
            or not raw_id
            or not input_id
        ):
            raise ValueError("invalid contribution checkpoint identity/count")
        if not isinstance(obligations, list) or any(
            not isinstance(item, str) or not item for item in obligations
        ):
            raise ValueError("invalid contribution checkpoint obligations")
        if obligations != sorted(set(obligations)):
            raise ValueError("noncanonical contribution checkpoint obligations")
        if self.pending_sort_key == sort_key:
            if self.pending_declaration_prefix != declaration_prefix:
                raise ValueError(
                    "one raw evidence sort key has conflicting declaration"
                )
            self.pending_occurrences += occurrences
            if self.pending_occurrences >= 1 << 64:
                raise ValueError("raw evidence occurrence overflow")
        else:
            self._flush_declaration()
            self.pending_sort_key = sort_key
            self.pending_declaration_prefix = declaration_prefix
            self.pending_occurrences = occurrences
        self.declared_count += occurrences
        self.contract_count += 1
        for identifier_digest, identifier in (
            (self.raw_id_digest, raw_id),
            (self.input_id_digest, input_id),
        ):
            encoded = identifier.encode("utf-8")
            identifier_digest.update(len(encoded).to_bytes(8, "big"))
            identifier_digest.update(encoded)
        self.obligations.update(obligations)


def mapping_context(
    required_rows: list[dict[str, str]], decode_rows: list[dict[str, str]]
) -> MappingContext:
    allocations = parse_patterns(
        required_rows,
        ("architecture_constraints", "decode_constraints"),
        (("decode_mask", "decode_value"),),
    )
    decode_patterns = parse_patterns(decode_rows)
    hierarchy: dict[int, list[Pattern]] = defaultdict(list)
    tables: dict[str, list[Pattern]] = defaultdict(list)
    for prefix, items in decode_patterns.items():
        hierarchy[prefix].extend(
            item for item in items if item.row["decode_layer"] == "hierarchy"
        )
    for row in decode_rows:
        if row["decode_layer"] != "instruction-table":
            continue
        tables[row["decode_class"]].append(
            Pattern(
                row,
                int(row["architecture_mask"], 16),
                int(row["architecture_value"], 16),
                parse_constraints(row["architecture_constraints"]),
            )
        )
    decode_id_by_identity = {
        (
            row["decode_class"],
            row["partition_name"],
            row["architecture_file"],
            row["architecture_mask"],
            row["architecture_value"],
            row["architecture_constraints"],
        ): row["partition_id"]
        for row in decode_rows
        if row["decode_layer"] == "instruction-table"
    }
    return MappingContext(
        allocations=allocations,
        hierarchy=hierarchy,
        tables=tables,
        decode_id_by_identity=decode_id_by_identity,
    )


def map_raw_contract(
    raw: dict[str, str], context: MappingContext
) -> list[MappedIdentity]:
    """Repeat the aggregate generator's exact official identity mapping."""
    word = int(raw["encoding"], 16)
    matches = select_allocations(context.allocations[word >> 20], word)
    declared_features = exact_features(raw["native_required_features"])
    allocation_features: dict[
        int,
        tuple[
            str,
            set[frozenset[str]],
            frozenset[str],
            frozenset[str],
        ],
    ] = {}
    for allocation in matches:
        expected = alternatives_text(allocation.row, word)
        allocation_features[id(allocation)] = (
            expected,
            {exact_features(item) for item in expected.split("|")},
            semantic_features_for_word(allocation.row, word),
            fallback_features(allocation.row, word),
        )
    some_allocation_matches_features = any(
        any(
            (
                required.issubset(declared_features)
                and declared_features - required <= semantic
            )
            or (
                required - declared_features <= fallback
                and declared_features - required <= semantic
            )
            for required in alternatives
        )
        for _text, alternatives, semantic, fallback in allocation_features.values()
    )
    if matches:
        mapped = [(pattern, "allocated") for pattern in matches]
    else:
        mapped = [
            (
                select_decode_partition(
                    context.hierarchy, context.tables, word
                ),
                "decode",
            )
        ]

    producer, source_site, source_file, source_line = source_identity(raw)
    result: list[MappedIdentity] = []
    for pattern, mapping_kind in mapped:
        required = pattern.row
        if mapping_kind == "allocated":
            validity = (
                "allocated-valid"
                if raw["invalid"] == "0"
                else "allocated-profile-illegal"
            )
            (
                expected_alternatives,
                expected_sets,
                semantic_features,
                native_fallback_features,
            ) = allocation_features[id(pattern)]
            semantically_related = any(
                feature_set.issubset(declared_features)
                and declared_features - feature_set <= semantic_features
                for feature_set in expected_sets
            )
            feature_fallback = any(
                feature_set - declared_features <= native_fallback_features
                and declared_features - feature_set <= semantic_features
                for feature_set in expected_sets
            )
            if raw["invalid"] == "1":
                native_status = "not-applicable-invalid"
            elif declared_features in expected_sets:
                native_status = "exact"
            elif semantically_related:
                native_status = "semantic-feature-gate"
            elif feature_fallback:
                native_status = "architectural-feature-fallback"
            else:
                native_status = "drift"
            if native_status == "drift" and some_allocation_matches_features:
                native_status = "overlapping-allocation"
            if (
                native_status == "drift"
                and raw["native_requirement_scope"] == "program"
            ):
                native_status = "program-level-gate"
            decode_partition_id = context.decode_id_by_identity.get(
                (
                    required["decode_class"],
                    required["decode_encoding"],
                    required["decode_architecture_file"],
                    required["decode_mask"],
                    required["decode_value"],
                    required["decode_constraints"],
                )
            )
            if decode_partition_id is None:
                raise ValueError("required allocation lacks decode partition")
            stable_ids = [required["stable_id"]]
            allocation_kind = required["allocation_kind"]
        else:
            validity = (
                required["partition_kind"]
                if raw["invalid"] == "1"
                else required["partition_kind"] + "-declared-valid"
            )
            expected_alternatives = (
                required["feature_condition"]
                if required["feature_condition"] != "baseline"
                else "baseline"
            )
            semantic_features = frozenset()
            native_fallback_features = frozenset()
            native_status = "not-applicable"
            decode_partition_id = required["partition_id"]
            stable_ids = (
                required["related_stable_ids"].split("+")
                if required["related_stable_ids"] != "none"
                else ["none"]
            )
            allocation_kind = "reserved"

        for stable_id in stable_ids:
            fields = {
                "stable_id": stable_id,
                "producer": producer,
                "source_site": source_site,
                "source_file": source_file,
                "source_line": source_line,
                "architecture_encoding": required.get(
                    "architecture_encoding",
                    required.get("partition_name", "none"),
                ),
                "architecture_mask": required["architecture_mask"],
                "architecture_value": required["architecture_value"],
                "architecture_constraints": required["architecture_constraints"],
                "decode_partition_id": decode_partition_id,
                "decode_class": required["decode_class"],
                "allocation_kind": allocation_kind,
                "validity_partition": validity,
                "expected_native_feature_alternatives": expected_alternatives,
                "native_requirement_status": native_status,
                "semantic_native_features": (
                    "+".join(sorted(semantic_features)) or "baseline"
                ),
                "native_fallback_features": (
                    "+".join(sorted(native_fallback_features)) or "baseline"
                ),
                "native_required_features": raw["native_required_features"],
                "native_requirement_scope": raw["native_requirement_scope"],
                "guest_page_size": raw["guest_page_size"],
                "expected_event": raw["expected_event"],
                "expected_retirement": raw["expected_retirement"],
                "expected_state_oracle": raw["expected_state_oracle"],
                "expected_fault_partition": fault_partition(
                    raw["expected_fault"]
                ),
            }
            if set(fields) != set(AGGREGATE_ID_COLUMNS):
                raise AssertionError("aggregate identity projection drifted")
            result.append(MappedIdentity(aggregate_id(fields), fields))
    identities = [item.aggregate_id for item in result]
    if len(identities) != len(set(identities)):
        raise ValueError(f"{raw['encoding']}: duplicate aggregate identity mapping")
    return result


DECLARED_COLUMNS = (
    "case",
    "encoding",
    "invalid",
    "guest_page_size",
    "native_required_features",
    "native_requirement_scope",
    "initial_state_digest",
    "initial_memory_digest",
    "expected_event",
    "expected_retirement",
    "expected_state_oracle",
    "expected_state_digest",
    "expected_memory_digest",
    "expected_fault",
    "expected_syndrome",
    "occurrences",
)


def declaration(raw: dict[str, str]) -> bytes:
    return "\0".join(raw[column] for column in DECLARED_COLUMNS).encode() + b"\n"


def evidence_sort_key(raw: dict[str, str]) -> tuple[object, ...]:
    """Match the raw C++ EvidenceKey ordering used by ``*.tsv.raw``."""
    return (
        raw["ctest"],
        raw["source"],
        raw["case"],
        int(raw["line"]),
        int(raw["encoding"], 16),
        int(raw["guest_page_size"]),
        int(raw["invalid"]),
        ENGINE_ORDER[raw["interpreter_result"]],
        ENGINE_ORDER[raw["gadgets_result"]],
        ENGINE_ORDER[raw["jit_result"]],
        raw["native_required_features"],
        SCOPE_ORDER[raw["native_requirement_scope"]],
        raw["native_observed_features"],
        raw["native_missing_features"],
        raw["expected_event"],
        raw["expected_retirement"],
        raw["expected_state_oracle"],
        int(raw["initial_state_digest"], 16),
        int(raw["initial_memory_digest"], 16),
        int(raw["expected_state_digest"], 16),
        int(raw["expected_memory_digest"], 16),
        raw["expected_fault"],
        int(raw["expected_syndrome"], 16),
    )


def _sort_text(value: str) -> bytes:
    """Encode a byte string so tuple-prefix ordering remains memcomparable."""
    return value.encode("utf-8").replace(b"\0", b"\0\xff") + b"\0\0"


def _sort_unsigned(value: int, width: int) -> bytes:
    if value < 0 or value >= 1 << (width * 8):
        raise ValueError("evidence sort-key integer is out of range")
    return value.to_bytes(width, "big")


def evidence_sort_key_blob(raw: dict[str, str]) -> bytes:
    """Return a SQLite-sortable encoding of the native EvidenceKey tuple."""
    parts = [
        _sort_text(raw["ctest"]),
        _sort_text(raw["source"]),
        _sort_text(raw["case"]),
        _sort_unsigned(int(raw["line"]), 4),
        _sort_unsigned(int(raw["encoding"], 16), 4),
        _sort_unsigned(int(raw["guest_page_size"]), 4),
        _sort_unsigned(int(raw["invalid"]), 1),
        _sort_unsigned(ENGINE_ORDER[raw["interpreter_result"]], 1),
        _sort_unsigned(ENGINE_ORDER[raw["gadgets_result"]], 1),
        _sort_unsigned(ENGINE_ORDER[raw["jit_result"]], 1),
        _sort_text(raw["native_required_features"]),
        _sort_unsigned(SCOPE_ORDER[raw["native_requirement_scope"]], 1),
        _sort_text(raw["native_observed_features"]),
        _sort_text(raw["native_missing_features"]),
        _sort_text(raw["expected_event"]),
        _sort_text(raw["expected_retirement"]),
        _sort_text(raw["expected_state_oracle"]),
        _sort_unsigned(int(raw["initial_state_digest"], 16), 8),
        _sort_unsigned(int(raw["initial_memory_digest"], 16), 8),
        _sort_unsigned(int(raw["expected_state_digest"], 16), 8),
        _sort_unsigned(int(raw["expected_memory_digest"], 16), 8),
        _sort_text(raw["expected_fault"]),
        _sort_unsigned(int(raw["expected_syndrome"], 16), 4),
    ]
    return b"".join(parts)


class ContributionSpool:
    """Disk-backed exact contribution ordering with constant Python payload RAM."""

    def __init__(
        self,
        root: Path,
        maximum_bytes: int | None = None,
        *,
        connection_factory: Callable[[Path], Any] = sqlite3.connect,
    ) -> None:
        self._root = root
        self._requested_maximum_bytes = maximum_bytes
        self._connection_factory = connection_factory
        self._maximum_bytes: int | None = None
        self._temporary: tempfile.TemporaryDirectory[str] | None = None
        self._database: sqlite3.Connection | None = None
        self._path: Path | None = None

    def __enter__(self) -> "ContributionSpool":
        if self._root.is_symlink() or not self._root.is_dir():
            raise ValueError(
                f"contribution spool root is not a regular directory: "
                f"{self._root}"
            )
        self._temporary = tempfile.TemporaryDirectory(
            prefix="a64-materialization-join-", dir=self._root
        )
        path = Path(self._temporary.name) / "contributions.sqlite3"
        self._path = path
        try:
            self._database = self._connection_factory(path)
            self._database.execute("PRAGMA journal_mode=OFF")
            self._database.execute("PRAGMA synchronous=OFF")
            self._database.execute("PRAGMA temp_store=FILE")
            self._database.execute("PRAGMA page_size=4096")
            if self._requested_maximum_bytes is not None:
                if self._requested_maximum_bytes <= 0:
                    raise ValueError("contribution spool cap must be positive")
                pages = self._requested_maximum_bytes // 4096
                if pages == 0:
                    raise ValueError("contribution spool cap is below one page")
                observed = int(
                    self._database.execute(
                        f"PRAGMA max_page_count={pages}"
                    ).fetchone()[0]
                )
                self._maximum_bytes = observed * 4096
            self._database.execute(
                """
                CREATE TABLE contribution (
                    aggregate_id TEXT NOT NULL,
                    sort_key BLOB NOT NULL,
                    declaration BLOB NOT NULL,
                    occurrences TEXT NOT NULL,
                    raw_id TEXT NOT NULL,
                    input_id TEXT NOT NULL,
                    obligations TEXT NOT NULL
                )
                """
            )
            # Build every B-tree while the table is empty. Inserts then grow
            # the rooted, max-page-count-capped database incrementally and no
            # index-sort temporary file can escape to a system temp volume.
            self._database.execute(
                "CREATE INDEX contribution_order "
                "ON contribution (sort_key, aggregate_id, raw_id)"
            )
            self._database.execute(
                "CREATE INDEX contribution_summary "
                "ON contribution (aggregate_id, sort_key, raw_id)"
            )
            self._database.execute(
                "CREATE UNIQUE INDEX contribution_raw_identity "
                "ON contribution (aggregate_id, raw_id)"
            )
            self._database.execute(
                "CREATE UNIQUE INDEX contribution_input_identity "
                "ON contribution (aggregate_id, input_id)"
            )
        except (sqlite3.DatabaseError, ValueError) as error:
            message = (
                self._storage_failure_message("initialization", error)
                if isinstance(error, sqlite3.DatabaseError)
                else None
            )
            if self._database is not None:
                self._database.close()
                self._database = None
            self._temporary.cleanup()
            self._temporary = None
            self._path = None
            if isinstance(error, ValueError):
                raise
            raise ValueError(message) from error
        return self

    def __exit__(self, *_args: object) -> None:
        if self._database is not None:
            self._database.close()
            self._database = None
        if self._temporary is not None:
            self._temporary.cleanup()
            self._temporary = None
        self._path = None

    @property
    def path(self) -> Path:
        if self._path is None:
            raise RuntimeError("contribution spool is not open")
        return self._path

    @property
    def database(self) -> sqlite3.Connection:
        if self._database is None:
            raise RuntimeError("contribution spool is not open")
        return self._database

    @property
    def maximum_bytes(self) -> int | None:
        return self._maximum_bytes

    def _storage_failure_message(
        self, operation: str, error: sqlite3.DatabaseError
    ) -> str:
        """Describe SQLite failure without misreporting every error as a cap hit."""
        code = getattr(error, "sqlite_errorcode", None)
        if code != sqlite3.SQLITE_FULL:
            return f"contribution spool SQLite {operation} failed: {error}"

        size = 0
        if self._path is not None:
            try:
                size = self._path.stat().st_size
            except OSError:
                pass
        try:
            free = shutil.disk_usage(self._root).free
        except OSError:
            free = -1
        remaining = (
            max(self._maximum_bytes - size, 0)
            if self._maximum_bytes is not None
            else None
        )
        proximity = SQLITE_CAP_PROXIMITY_PAGES * 4096
        if (
            remaining is not None
            and remaining <= proximity
            and free > remaining + proximity
        ):
            return (
                "contribution spool reached its hard byte cap "
                f"during {operation} (database={size}, cap={self._maximum_bytes})"
            )
        if free >= 0 and free <= proximity:
            return (
                "contribution spool backing filesystem is full "
                f"during {operation} (database={size}, free={free})"
            )
        return (
            "contribution spool database or filesystem capacity was exhausted "
            f"during {operation} (database={size}, cap="
            f"{self._maximum_bytes if self._maximum_bytes is not None else 'none'}, "
            f"free={free if free >= 0 else 'unknown'})"
        )

    def add(
        self,
        aggregate_id: str,
        raw: dict[str, str],
        raw_id: str,
        input_id: str,
        obligations: tuple[str, ...],
    ) -> None:
        try:
            self.database.execute(
                "INSERT INTO contribution VALUES (?, ?, ?, ?, ?, ?, ?)",
                (
                    aggregate_id,
                    evidence_sort_key_blob(raw),
                    declaration(raw),
                    raw["occurrences"],
                    raw_id,
                    input_id,
                    json.dumps(obligations, separators=(",", ":")),
                ),
            )
        except sqlite3.IntegrityError as error:
            raise ValueError(
                "duplicate raw/input contract mapping reached contribution spool"
            ) from error
        except sqlite3.DatabaseError as error:
            raise ValueError(self._storage_failure_message("insert", error)) from error

    def finish(self) -> None:
        try:
            self.database.commit()
        except sqlite3.DatabaseError as error:
            raise ValueError(self._storage_failure_message("commit", error)) from error
        if (
            self.maximum_bytes is not None
            and self.path.stat().st_size > self.maximum_bytes
        ):
            raise ValueError("contribution spool exceeded its hard byte cap")

    def query_plan(
        self, sql: str, parameters: tuple[object, ...] = ()
    ) -> list[str]:
        return [
            str(row[3])
            for row in self.database.execute(
                "EXPLAIN QUERY PLAN " + sql, parameters
            )
        ]

    def identities(self) -> set[str]:
        return {
            str(row[0])
            for row in self.database.execute(
                "SELECT DISTINCT aggregate_id FROM contribution"
            )
        }

    def summary(
        self, aggregate_id: str
    ) -> tuple[int, int, str, int, str, int, str, list[str]]:
        accumulator = AggregateAccumulator()
        cursor = self.database.execute(
            "SELECT sort_key, declaration, occurrences, raw_id, input_id, "
            "obligations FROM contribution WHERE aggregate_id = ? "
            "ORDER BY sort_key, raw_id",
            (aggregate_id,),
        )
        for (
            sort_key,
            raw_declaration,
            occurrences,
            raw_id,
            input_id,
            encoded,
        ) in cursor:
            accumulator.add(
                {
                    "sort_key": bytes(sort_key).hex(),
                    "declaration_base64": base64.b64encode(
                        bytes(raw_declaration)
                    ).decode("ascii"),
                    "raw_occurrences": str(occurrences),
                    "raw_contract_id": str(raw_id),
                    "input_materialization_id": str(input_id),
                    "representation_obligations": json.loads(str(encoded)),
                }
            )
        accumulator.finish()
        return (
            accumulator.contract_count,
            accumulator.declared_count,
            accumulator.declared_digest.hexdigest(),
            accumulator.contract_count,
            accumulator.raw_id_digest.hexdigest(),
            accumulator.contract_count,
            accumulator.input_id_digest.hexdigest(),
            sorted(accumulator.obligations),
        )

    def iter_ordered(self) -> Iterator[dict[str, object]]:
        cursor = self.database.execute(
            "SELECT aggregate_id, sort_key, declaration, occurrences, "
            "raw_id, input_id, obligations FROM contribution "
            "ORDER BY sort_key, aggregate_id, raw_id"
        )
        for (
            aggregate_id,
            sort_key,
            raw_declaration,
            occurrences,
            raw_id,
            input_id,
            encoded,
        ) in cursor:
            obligations = list(json.loads(str(encoded)))
            yield {
                "schema": CONTRACT_INDEX_SCHEMA,
                "aggregate_id": str(aggregate_id),
                "sort_key": bytes(sort_key).hex(),
                "declaration_base64": base64.b64encode(
                    bytes(raw_declaration)
                ).decode("ascii"),
                "raw_contract_id": str(raw_id),
                "input_materialization_id": str(input_id),
                "raw_occurrences": str(occurrences),
                "representation_obligations": obligations,
            }


def _sha256_path(path: Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
            size += len(chunk)
    return size, digest.hexdigest()


def _update_framed(digest: Any, payload: bytes) -> None:
    digest.update(len(payload).to_bytes(8, "big"))
    digest.update(payload)


def join_model_digest(
    aggregates: Iterable[Mapping[str, str]],
    required: Iterable[Mapping[str, str]],
    decodes: Iterable[Mapping[str, str]],
    callback_registry: Mapping[str, Mapping[str, str]],
) -> str:
    """Bind resumable contribution shards to every mapping/projection input."""
    digest = hashlib.sha256()
    _update_framed(digest, JOIN_MODEL_SCHEMA.encode("ascii"))
    for label, rows in (
        ("aggregates", aggregates),
        ("required", required),
        ("decodes", decodes),
    ):
        _update_framed(digest, label.encode("ascii"))
        for row in rows:
            _update_framed(digest, canonical_json(dict(row)).encode("utf-8"))
    _update_framed(digest, b"callback-registry")
    for descriptor_id in sorted(callback_registry):
        _update_framed(
            digest,
            canonical_json(dict(callback_registry[descriptor_id])).encode(
                "utf-8"
            ),
        )
    _update_framed(
        digest, canonical_json(projection_inventory()).encode("utf-8")
    )
    return digest.hexdigest()


@contextmanager
def _gzip_text(
    path: Path, maximum_bytes: int | None = None
) -> Iterator[TextIO]:
    """Write a gzip member while enforcing the optional output-size cap."""
    with path.open("wb") as raw:
        destination: Any = raw
        if maximum_bytes is not None:
            if maximum_bytes <= 0:
                raise ValueError("checkpoint cap must be positive")

            class BoundedWriter:
                def write(self, payload: bytes) -> int:
                    if raw.tell() + len(payload) > maximum_bytes:
                        raise ValueError(
                            "contribution checkpoint exceeded its hard byte cap"
                        )
                    return raw.write(payload)

                def flush(self) -> None:
                    raw.flush()

                def tell(self) -> int:
                    return raw.tell()

            destination = BoundedWriter()
        with gzip.GzipFile(
            mode="wb", compresslevel=6, fileobj=destination
        ) as compressed:
            with io.TextIOWrapper(
                compressed, encoding="utf-8", newline=""
            ) as output:
                yield output


def _unique_staging_path(path: Path) -> Path:
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    os.close(descriptor)
    return Path(temporary_name)


def _checkpoint_line(record: Mapping[str, object]) -> str:
    return canonical_json(dict(record)) + "\n"


def _checkpoint_filename(
    source: Path, source_sha256: str, model_digest: str
) -> str:
    safe_name = re.sub(r"[^A-Za-z0-9_.-]+", "_", source.name)[:48]
    return (
        f"{safe_name}.{source_sha256}.{model_digest}."
        "contributions.jsonl.gz"
    )


def write_contribution_checkpoint(
    path: Path,
    header: Mapping[str, object],
    spool: ContributionSpool,
    maximum_bytes: int | None = None,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = _unique_staging_path(path)
    row_digest = hashlib.sha256()
    row_count = 0
    try:
        with _gzip_text(temporary, maximum_bytes) as output:
            output.write(_checkpoint_line(header))
            for row in spool.iter_ordered():
                encoded = _checkpoint_line(row)
                _update_framed(row_digest, encoded.encode("utf-8"))
                output.write(encoded)
                row_count += 1
            output.write(
                _checkpoint_line(
                    {
                        "schema": CONTRIBUTION_CHECKPOINT_FOOTER_SCHEMA,
                        "contribution_count": str(row_count),
                        "contribution_digest": row_digest.hexdigest(),
                    }
                )
            )
        if maximum_bytes is not None and temporary.stat().st_size > maximum_bytes:
            raise ValueError(
                "contribution checkpoint exceeded its hard byte cap"
            )
        verify_contribution_checkpoint(temporary, expected_header=header)
        os.replace(temporary, path)
        verify_contribution_checkpoint(path, expected_header=header)
    finally:
        if temporary.exists():
            temporary.unlink()


def _decode_checkpoint_record(
    path: Path, line_number: int, line: bytes
) -> dict[str, object]:
    try:
        text = line.decode("utf-8")
        record = json.loads(text)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(
            f"{path}:{line_number}: invalid checkpoint JSON"
        ) from error
    if not isinstance(record, dict) or _checkpoint_line(record).encode() != line:
        raise ValueError(
            f"{path}:{line_number}: noncanonical checkpoint record"
        )
    return record


def verify_contribution_checkpoint(
    path: Path,
    *,
    expected_header: Mapping[str, object] | None = None,
    consume: Callable[[Mapping[str, object]], None] | None = None,
) -> CheckpointInfo:
    """Verify gzip CRC/EOF, source/model binding, order, and logical digest."""
    header: dict[str, object] | None = None
    footer: dict[str, object] | None = None
    row_count = 0
    row_digest = hashlib.sha256()
    previous_key: tuple[bytes, str, str] | None = None
    try:
        with gzip.open(path, "rb") as source:
            for line_number, line in enumerate(source, 1):
                record = _decode_checkpoint_record(path, line_number, line)
                schema = record.get("schema")
                if line_number == 1:
                    expected_keys = {
                        "schema",
                        "producer",
                        "source_name",
                        "source_sha256",
                        "source_size",
                        "join_model_digest",
                        "guest_page_size",
                    }
                    if (
                        schema != CONTRIBUTION_CHECKPOINT_HEADER_SCHEMA
                        or set(record) != expected_keys
                    ):
                        raise ValueError(f"{path}: invalid checkpoint header")
                    if expected_header is not None and record != dict(
                        expected_header
                    ):
                        raise ValueError(f"{path}: checkpoint header binding differs")
                    try:
                        source_size = int(str(record["source_size"]))
                        page_size = int(str(record["guest_page_size"]))
                    except ValueError as error:
                        raise ValueError(
                            f"{path}: invalid checkpoint header integer"
                        ) from error
                    if (
                        source_size <= 0
                        or page_size not in (4096, 16384)
                        or not str(record["producer"])
                        or not str(record["source_name"])
                        or not re.fullmatch(
                            r"[0-9a-f]{64}", str(record["source_sha256"])
                        )
                        or not re.fullmatch(
                            r"[0-9a-f]{64}",
                            str(record["join_model_digest"]),
                        )
                    ):
                        raise ValueError(f"{path}: invalid checkpoint header value")
                    header = record
                    continue
                if schema == CONTRIBUTION_CHECKPOINT_FOOTER_SCHEMA:
                    if footer is not None or set(record) != {
                        "schema",
                        "contribution_count",
                        "contribution_digest",
                    }:
                        raise ValueError(f"{path}: invalid checkpoint footer")
                    footer = record
                    continue
                if footer is not None:
                    raise ValueError(f"{path}: records follow checkpoint footer")
                if schema != CONTRACT_INDEX_SCHEMA or set(record) != set(
                    CONTRIBUTION_CHECKPOINT_COLUMNS
                ):
                    raise ValueError(
                        f"{path}:{line_number}: invalid checkpoint contribution"
                    )
                try:
                    sort_key = bytes.fromhex(str(record["sort_key"]))
                    base64.b64decode(
                        str(record["declaration_base64"]), validate=True
                    )
                    occurrences = int(str(record["raw_occurrences"]))
                except (ValueError, binascii.Error) as error:
                    raise ValueError(
                        f"{path}:{line_number}: malformed contribution value"
                    ) from error
                obligations = record["representation_obligations"]
                if (
                    not sort_key
                    or occurrences <= 0
                    or not str(record["aggregate_id"])
                    or not str(record["raw_contract_id"])
                    or not str(record["input_materialization_id"])
                    or not isinstance(obligations, list)
                    or obligations != sorted(set(obligations))
                    or any(
                        not isinstance(item, str) or not item
                        for item in obligations
                    )
                ):
                    raise ValueError(
                        f"{path}:{line_number}: invalid contribution value"
                    )
                current_key = (
                    sort_key,
                    str(record["aggregate_id"]),
                    str(record["raw_contract_id"]),
                )
                if previous_key is not None and current_key <= previous_key:
                    raise ValueError(f"{path}: checkpoint order is not strict")
                previous_key = current_key
                encoded = _checkpoint_line(record).encode("utf-8")
                _update_framed(row_digest, encoded)
                row_count += 1
                if consume is not None:
                    consume(record)
    except (EOFError, OSError, zlib.error) as error:
        raise ValueError(f"{path}: incomplete/corrupt gzip checkpoint") from error
    if header is None or footer is None:
        raise ValueError(f"{path}: incomplete checkpoint")
    try:
        footer_count = int(str(footer["contribution_count"]))
    except ValueError as error:
        raise ValueError(f"{path}: invalid checkpoint footer count") from error
    if (
        footer_count != row_count
        or footer["contribution_digest"] != row_digest.hexdigest()
    ):
        raise ValueError(f"{path}: checkpoint footer integrity differs")
    checkpoint_size, checkpoint_sha256 = _sha256_path(path)
    return CheckpointInfo(
        path=path,
        producer=str(header["producer"]),
        source_name=str(header["source_name"]),
        source_sha256=str(header["source_sha256"]),
        source_size=int(str(header["source_size"])),
        join_model_digest=str(header["join_model_digest"]),
        guest_page_size=int(str(header["guest_page_size"])),
        contribution_count=row_count,
        contribution_digest=row_digest.hexdigest(),
        checkpoint_sha256=checkpoint_sha256,
        checkpoint_size=checkpoint_size,
    )


def build_or_reuse_contribution_checkpoint(
    source: Path,
    checkpoint_directory: Path,
    *,
    aggregate_by_id: Mapping[str, Mapping[str, str]],
    scoped_ids: set[str],
    context: MappingContext,
    callback_registry: Mapping[str, Mapping[str, str]],
    model_digest: str,
    guest_page_size: int | None,
    spool_maximum_bytes: int | None = None,
    checkpoint_maximum_bytes: int | None = None,
) -> CheckpointInfo:
    """Map exactly one producer into a verified, resumable compressed shard."""
    reusable = reusable_contribution_checkpoint(
        source,
        checkpoint_directory,
        model_digest=model_digest,
        guest_page_size=guest_page_size,
    )
    if reusable is not None:
        return reusable
    source_size, source_sha256 = _sha256_path(source)
    checkpoint = checkpoint_directory / _checkpoint_filename(
        source, source_sha256, model_digest
    )
    if checkpoint.exists() or checkpoint.is_symlink():
        if checkpoint.is_symlink() or not checkpoint.is_file():
            raise ValueError(
                f"{checkpoint}: resumable checkpoint is not a regular file"
            )
        try:
            info = verify_contribution_checkpoint(checkpoint)
        except ValueError:
            checkpoint.unlink()
        else:
            if (
                info.source_name != source.name
                or info.source_sha256 != source_sha256
                or info.source_size != source_size
                or info.join_model_digest != model_digest
                or (
                    guest_page_size is not None
                    and info.guest_page_size != guest_page_size
                )
            ):
                raise ValueError(
                    f"{checkpoint}: resumable checkpoint binding differs"
                )
            return info

    producer: str | None = None
    producer_page_size: int | None = None
    with ContributionSpool(
        checkpoint_directory, maximum_bytes=spool_maximum_bytes
    ) as spool:
        for record in iter_raw_materializations([source]):
            validate_callback_contract_registry_use(record, callback_registry)
            raw = raw_contract_tsv(record)
            page_size = int(raw["guest_page_size"])
            if guest_page_size is not None and page_size != guest_page_size:
                raise ValueError(
                    f"{raw['ctest']}: sidecar page size {page_size} differs "
                    f"from requested profile {guest_page_size}"
                )
            if producer is None:
                producer = raw["ctest"]
                producer_page_size = page_size
            elif producer != raw["ctest"] or producer_page_size != page_size:
                raise ValueError(
                    f"{source}: one contribution shard must contain exactly "
                    "one ctest/page-size producer"
                )
            mapped = map_raw_contract(raw, context)
            raw_id = raw_contract_id(record)
            input_id = materialization_id(record)
            obligations = representation_obligations(record)
            for mapped_identity in mapped:
                identity = mapped_identity.aggregate_id
                manifest = aggregate_by_id.get(identity)
                if manifest is None or identity not in scoped_ids:
                    raise ValueError(
                        f"{source}: {raw_id} maps to unknown/out-of-profile "
                        f"{identity}"
                    )
                changed = [
                    column
                    for column in AGGREGATE_ID_COLUMNS
                    if manifest[column] != mapped_identity.fields[column]
                ]
                if changed:
                    raise ValueError(
                        f"{identity}: manifest identity fields differ: "
                        + ",".join(changed)
                    )
                spool.add(identity, raw, raw_id, input_id, obligations)
        if producer is None or producer_page_size is None:
            raise ValueError(f"{source}: input materialization is empty")
        checked_size, checked_sha256 = _sha256_path(source)
        if (checked_size, checked_sha256) != (source_size, source_sha256):
            raise ValueError(f"{source}: source changed while checkpointing")
        spool.finish()
        header: dict[str, object] = {
            "schema": CONTRIBUTION_CHECKPOINT_HEADER_SCHEMA,
            "producer": producer,
            "source_name": source.name,
            "source_sha256": source_sha256,
            "source_size": str(source_size),
            "join_model_digest": model_digest,
            "guest_page_size": str(producer_page_size),
        }
        write_contribution_checkpoint(
            checkpoint,
            header,
            spool,
            maximum_bytes=checkpoint_maximum_bytes,
        )
    return verify_contribution_checkpoint(checkpoint, expected_header=header)


def reusable_contribution_checkpoint(
    source: Path,
    checkpoint_directory: Path,
    *,
    model_digest: str,
    guest_page_size: int | None,
) -> CheckpointInfo | None:
    """Return an exact reusable shard without requiring rebuild headroom."""
    source_size, source_sha256 = _sha256_path(source)
    checkpoint = checkpoint_directory / _checkpoint_filename(
        source, source_sha256, model_digest
    )
    if not checkpoint.exists() and not checkpoint.is_symlink():
        return None
    if checkpoint.is_symlink() or not checkpoint.is_file():
        raise ValueError(
            f"{checkpoint}: resumable checkpoint is not a regular file"
        )
    try:
        info = verify_contribution_checkpoint(checkpoint)
    except ValueError:
        return None
    if (
        info.source_name != source.name
        or info.source_sha256 != source_sha256
        or info.source_size != source_size
        or info.join_model_digest != model_digest
        or (
            guest_page_size is not None
            and info.guest_page_size != guest_page_size
        )
    ):
        raise ValueError(f"{checkpoint}: resumable checkpoint binding differs")
    checked_size, checked_sha256 = _sha256_path(source)
    if (checked_size, checked_sha256) != (source_size, source_sha256):
        raise ValueError(f"{source}: source changed while reusing checkpoint")
    return info


def write_contract_manifest(
    path: Path, checkpoints: Iterable[CheckpointInfo]
) -> None:
    checkpoint_rows = list(checkpoints)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = _unique_staging_path(path)
    try:
        if path.suffix == ".gz":
            context = _gzip_text(temporary)
        else:
            context = temporary.open("w", encoding="utf-8", newline="")
        with context as output:
            writer = csv.DictWriter(
                output,
                fieldnames=CONTRACT_MANIFEST_COLUMNS,
                delimiter="\t",
                lineterminator="\n",
            )
            writer.writeheader()
            for info in checkpoint_rows:
                relative = os.path.relpath(info.path, path.parent)
                writer.writerow(
                    {
                        "schema": CONTRACT_MANIFEST_SCHEMA,
                        "producer": info.producer,
                        "checkpoint_file": relative,
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
        verify_contract_manifest(
            temporary, checkpoint_rows, compressed=path.suffix == ".gz"
        )
        os.replace(temporary, path)
        verify_contract_manifest(path, checkpoint_rows)
    finally:
        if temporary.exists():
            temporary.unlink()


def contract_manifest_payload(
    path: Path, checkpoints: Iterable[CheckpointInfo]
) -> bytes:
    text = io.StringIO(newline="")
    writer = csv.DictWriter(
        text,
        fieldnames=CONTRACT_MANIFEST_COLUMNS,
        delimiter="\t",
        lineterminator="\n",
    )
    writer.writeheader()
    writer.writerows(_expected_contract_manifest_rows(path, checkpoints))
    plain = text.getvalue().encode("utf-8")
    if path.suffix != ".gz":
        return plain
    encoded = io.BytesIO()
    with gzip.GzipFile(
        mode="wb", compresslevel=6, fileobj=encoded
    ) as output:
        output.write(plain)
    return encoded.getvalue()


def _expected_contract_manifest_rows(
    path: Path, checkpoints: Iterable[CheckpointInfo]
) -> list[dict[str, str]]:
    return [
        {
            "schema": CONTRACT_MANIFEST_SCHEMA,
            "producer": info.producer,
            "checkpoint_file": os.path.relpath(info.path, path.parent),
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
        for info in checkpoints
    ]


def verify_contract_manifest(
    path: Path,
    checkpoints: Iterable[CheckpointInfo],
    *,
    compressed: bool | None = None,
) -> None:
    """Reopen and exact-compare a published or staged contract manifest."""
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"contract manifest is not a regular file: {path}")
    if compressed is None:
        compressed = path.suffix == ".gz"
    opener = gzip.open if compressed else open
    try:
        with opener(path, "rt", encoding="utf-8", newline="") as source:
            reader = csv.DictReader(source, delimiter="\t")
            if reader.fieldnames != CONTRACT_MANIFEST_COLUMNS:
                raise ValueError(f"{path}: contract manifest columns differ")
            actual = list(reader)
    except (EOFError, OSError, UnicodeDecodeError, zlib.error) as error:
        raise ValueError(f"{path}: contract manifest is corrupt") from error
    expected = _expected_contract_manifest_rows(path, checkpoints)
    if actual != expected:
        raise ValueError(f"{path}: contract manifest contents differ")


def _source_producer_hint(path: Path) -> str | None:
    name = path.name
    for suffix in (".tsv.inputs.jsonl.gz", ".tsv.inputs.jsonl"):
        if name.endswith(suffix):
            return name[: -len(suffix)]
    return None


def require_checkpoint_free_space(
    path: Path,
    source: Path,
    expected_contributions: int | None,
    *,
    reserve_bytes: int,
    spool_bytes_per_contribution: int,
    checkpoint_bytes_per_contribution: int,
) -> int:
    """Reject a producer before SQLite can consume the volume's reserve."""
    if (
        reserve_bytes < 0
        or spool_bytes_per_contribution <= 0
        or checkpoint_bytes_per_contribution <= 0
    ):
        raise ValueError("invalid checkpoint free-space policy")
    spool_cap, checkpoint_cap = producer_storage_caps(
        source,
        expected_contributions,
        spool_bytes_per_contribution=spool_bytes_per_contribution,
        checkpoint_bytes_per_contribution=checkpoint_bytes_per_contribution,
    )
    required = reserve_bytes + spool_cap + checkpoint_cap
    free = shutil.disk_usage(path).free
    if free < required:
        raise ValueError(
            f"{source}: insufficient free space for bounded producer "
            f"checkpoint (free={free}, required={required}, "
            f"reserve={reserve_bytes}, contributions="
            f"{expected_contributions if expected_contributions is not None else 'unknown'})"
        )
    return required


def producer_storage_caps(
    source: Path,
    expected_contributions: int | None,
    *,
    spool_bytes_per_contribution: int,
    checkpoint_bytes_per_contribution: int,
) -> tuple[int, int]:
    if (
        spool_bytes_per_contribution <= 0
        or checkpoint_bytes_per_contribution <= 0
        or (
            expected_contributions is not None
            and expected_contributions < 0
        )
    ):
        raise ValueError("invalid checkpoint storage-cap policy")
    source_size = source.stat().st_size
    if expected_contributions is None:
        # Explicit sidecars without a checked producer hint remain bounded by
        # their source size, with conservative independent working caps.
        spool_cap = max(512 * 1024 * 1024, source_size * 8)
        checkpoint_cap = max(64 * 1024 * 1024, source_size * 2)
    else:
        spool_cap = (
            expected_contributions * spool_bytes_per_contribution
            + SPOOL_METADATA_ALLOWANCE_BYTES
        )
        checkpoint_cap = (
            expected_contributions * checkpoint_bytes_per_contribution
            + CHECKPOINT_METADATA_ALLOWANCE_BYTES
        )
    return spool_cap, checkpoint_cap


def prune_contribution_checkpoints(
    directory: Path,
    active: Iterable[CheckpointInfo],
    total_checkpoint_cap: int,
) -> None:
    active_paths = {checkpoint.path.resolve() for checkpoint in active}
    active_size = sum(checkpoint.checkpoint_size for checkpoint in active)
    if active_size > total_checkpoint_cap:
        raise ValueError(
            "active contribution checkpoints exceed their aggregate hard cap"
        )
    for path in sorted(directory.glob("*.contributions.jsonl.gz")):
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"checkpoint artifact is not a regular file: {path}")
        if path.resolve() in active_paths:
            continue
        path.unlink()


def reserve_bytes_from_gib(value: float) -> int:
    if not math.isfinite(value) or value < 0:
        raise ValueError("free-space reserve must be finite and nonnegative")
    return int(value * 1024 * 1024 * 1024)


def materialization_paths(
    directories: Iterable[Path], explicit: Iterable[Path]
) -> list[Path]:
    paths = list(explicit)
    for directory in directories:
        paths.extend(sorted(directory.glob("*.tsv.inputs.jsonl")))
        paths.extend(sorted(directory.glob("*.tsv.inputs.jsonl.gz")))
    checked_paths: list[Path] = []
    logical_paths: list[tuple[Path, Path]] = []
    for original in paths:
        path = require_no_symlink_components(
            original, "input-materialization source"
        )
        if path.is_symlink() or not path.is_file():
            raise ValueError(
                f"input-materialization source is not a nonsymlink regular "
                f"file: {path}"
            )
        logical = (
            Path(str(path)[:-3])
            if path.name.endswith(".tsv.inputs.jsonl.gz")
            else path
        )
        for previous_logical, previous_path in logical_paths:
            if paths_alias(logical, previous_logical):
                raise ValueError(
                    "input-materialization source has duplicate plain/gzip "
                    f"forms: {previous_path}, {path}"
                )
        logical_paths.append((logical, path))
        checked_paths.append(path)
    unique: list[Path] = []
    for path in checked_paths:
        if any(paths_alias(path, previous) for previous in unique):
            raise ValueError(
                f"input-materialization source is duplicated: {path}"
            )
        unique.append(path)
    unique.sort()
    if not unique:
        raise ValueError("no A64 input-materialization JSONL files found")
    return unique


def validate_join_output_paths(
    output: Path,
    contract_index_output: Path,
    checkpoint_directory: Path,
    read_inputs: Iterable[Path],
) -> tuple[Path, Path, Path]:
    """Canonicalize every write target and reject input/output aliases."""
    original_targets = (output, contract_index_output, checkpoint_directory)
    targets = tuple(
        require_no_symlink_components(path, "materialization join write target")
        for path in original_targets
    )
    sources = tuple(
        require_no_symlink_components(path, "materialization join read input")
        for path in read_inputs
    )
    labels = ("output", "contract index", "checkpoint directory")
    for label, original in zip(labels, original_targets):
        if original.is_symlink():
            raise ValueError(f"materialization join {label} is a symlink")
    for index, left in enumerate(targets):
        for right in targets[index + 1 :]:
            if paths_alias(left, right):
                raise ValueError("materialization join write targets alias")
    for index, left in enumerate(targets):
        for right in targets[index + 1 :]:
            if path_is_ancestor(left, right) or path_is_ancestor(right, left):
                raise ValueError(
                    "materialization join write targets have impossible ancestry"
                )
    for label, target in zip(labels, targets):
        if label == "checkpoint directory":
            if target.exists() and not target.is_dir():
                raise ValueError(
                    "materialization join checkpoint target is not a directory"
                )
        elif target.exists() and not target.is_file():
            raise ValueError(
                f"materialization join {label} is not a regular file"
            )
        for source in sources:
            if paths_alias(target, source):
                raise ValueError(
                    f"materialization join {label} aliases a read input"
                )
            if path_is_ancestor(source, target) or (
                label != "checkpoint directory"
                and path_is_ancestor(target, source)
            ):
                raise ValueError(
                    f"materialization join {label} has impossible read-input "
                    "ancestry"
                )
    return targets


def require_complete_producer_coverage(
    expected: Iterable[str], observed: Iterable[str]
) -> None:
    expected_set = set(expected)
    observed_set = set(observed)
    missing = sorted(expected_set - observed_set)
    extra = sorted(observed_set - expected_set)
    if missing or extra:
        details: list[str] = []
        if missing:
            details.append(
                f"{len(missing)} absent producers; first={missing[:3]}"
            )
        if extra:
            details.append(
                f"{len(extra)} unexpected producers; first={extra[:3]}"
            )
        raise ValueError("materialization producer coverage incomplete: " + "; ".join(details))


def validate_projection_inventory(path: Path) -> None:
    with path.open(encoding="utf-8") as source:
        checked = json.load(source)
    expected = projection_inventory()
    if checked != expected:
        raise ValueError(
            f"{path}: projection inventory differs from {canonical_json(expected)}"
        )


def join_materializations(
    paths: list[Path],
    aggregates: list[dict[str, str]],
    required: list[dict[str, str]],
    decodes: list[dict[str, str]],
    callback_registry: Mapping[str, Mapping[str, str]],
    *,
    guest_page_size: int | None = None,
    allow_partial: bool = False,
    require_execution_ready: bool = False,
    contract_index_output: Path | None = None,
    checkpoint_directory: Path | None = None,
    free_space_reserve_bytes: int = 0,
    spool_bytes_per_contribution: int = DEFAULT_SPOOL_BYTES_PER_CONTRIBUTION,
    checkpoint_bytes_per_contribution: int = 128,
    deferred_checkpoints: list[CheckpointInfo] | None = None,
) -> list[dict[str, str]]:
    if len(required) != 2396:
        raise ValueError("required allocation model is not 2,396 rows")
    aggregate_by_id = {row["aggregate_id"]: row for row in aggregates}
    if len(aggregate_by_id) != len(aggregates):
        raise ValueError("aggregate manifest IDs are not unique")
    plan = build_plan(aggregates, required, decodes)
    plan_by_id = {row["aggregate_id"]: row for row in plan}
    scoped_ids = {
        row["aggregate_id"]
        for row in aggregates
        if guest_page_size is None
        or int(row["guest_page_size"]) == guest_page_size
    }
    context = mapping_context(required, decodes)
    model_digest = join_model_digest(
        aggregates, required, decodes, callback_registry
    )
    output: list[dict[str, str]] = []
    failures: list[str] = []
    temporary_checkpoints: tempfile.TemporaryDirectory[str] | None = None
    if checkpoint_directory is None:
        if contract_index_output is not None:
            checkpoint_directory = contract_index_output.with_name(
                contract_index_output.name + ".d"
            )
        else:
            temporary_checkpoints = tempfile.TemporaryDirectory(
                prefix="a64-materialization-checkpoints-"
            )
            checkpoint_directory = Path(temporary_checkpoints.name)
    checkpoint_directory.mkdir(parents=True, exist_ok=True)
    try:
        declared_by_producer: dict[str, int] = defaultdict(int)
        for row in aggregates:
            if (
                guest_page_size is None
                or int(row["guest_page_size"]) == guest_page_size
            ):
                declared_by_producer[row["producer"]] += int(
                    row["declared_count"]
                )
        checkpoints: list[CheckpointInfo] = []
        total_checkpoint_cap = 0
        for path in paths:
            hint = _source_producer_hint(path)
            expected_contributions = (
                declared_by_producer.get(hint) if hint is not None else None
            )
            spool_cap, checkpoint_cap = producer_storage_caps(
                path,
                expected_contributions,
                spool_bytes_per_contribution=spool_bytes_per_contribution,
                checkpoint_bytes_per_contribution=(
                    checkpoint_bytes_per_contribution
                ),
            )
            total_checkpoint_cap += checkpoint_cap
            reusable = reusable_contribution_checkpoint(
                path,
                checkpoint_directory,
                model_digest=model_digest,
                guest_page_size=guest_page_size,
            )
            if reusable is not None:
                if reusable.checkpoint_size > checkpoint_cap:
                    raise ValueError(
                        f"{reusable.path}: reusable checkpoint exceeds its "
                        "current hard cap"
                    )
                checkpoints.append(reusable)
                continue
            require_checkpoint_free_space(
                checkpoint_directory,
                path,
                expected_contributions,
                reserve_bytes=free_space_reserve_bytes,
                spool_bytes_per_contribution=spool_bytes_per_contribution,
                checkpoint_bytes_per_contribution=(
                    checkpoint_bytes_per_contribution
                ),
            )
            checkpoints.append(
                build_or_reuse_contribution_checkpoint(
                    path,
                    checkpoint_directory,
                    aggregate_by_id=aggregate_by_id,
                    scoped_ids=scoped_ids,
                    context=context,
                    callback_registry=callback_registry,
                    model_digest=model_digest,
                    guest_page_size=guest_page_size,
                    spool_maximum_bytes=spool_cap,
                    checkpoint_maximum_bytes=checkpoint_cap,
                )
            )
        checkpoints.sort(key=lambda item: item.producer.encode("utf-8"))
        duplicate_producers = [
            checkpoints[index].producer
            for index in range(1, len(checkpoints))
            if checkpoints[index - 1].producer == checkpoints[index].producer
        ]
        if duplicate_producers:
            raise ValueError(
                "a producer is split across multiple sidecars; exact global "
                "EvidenceKey order requires one sidecar per producer: "
                + ",".join(duplicate_producers[:3])
            )
        if not allow_partial:
            require_complete_producer_coverage(
                declared_by_producer,
                (checkpoint.producer for checkpoint in checkpoints),
            )

        accumulators: dict[str, AggregateAccumulator] = {}

        def consume_contribution(row: Mapping[str, object]) -> None:
            identity = str(row["aggregate_id"])
            if identity not in scoped_ids or identity not in aggregate_by_id:
                raise ValueError(
                    f"checkpoint maps to unknown/out-of-profile {identity}"
                )
            accumulators.setdefault(identity, AggregateAccumulator()).add(row)

        for checkpoint in checkpoints:
            checked = verify_contribution_checkpoint(
                checkpoint.path, consume=consume_contribution
            )
            if checked != checkpoint:
                raise ValueError(
                    f"{checkpoint.path}: checkpoint changed after verification"
                )

        joined_ids = set(accumulators)
        missing = sorted(scoped_ids - joined_ids)
        if missing and not allow_partial:
            raise ValueError(
                f"materialization coverage incomplete: {len(missing)} aggregate "
                f"identities missing; first={missing[:3]}"
            )

        for identity in sorted(joined_ids):
            manifest = aggregate_by_id[identity]
            plan_row = plan_by_id[identity]
            accumulator = accumulators[identity]
            accumulator.finish()
            contract_count = accumulator.contract_count
            declared_count = accumulator.declared_count
            declared_digest = accumulator.declared_digest.hexdigest()
            raw_id_count = contract_count
            raw_id_digest = accumulator.raw_id_digest.hexdigest()
            input_id_count = contract_count
            input_id_digest = accumulator.input_id_digest.hexdigest()
            obligations = sorted(accumulator.obligations)
            if declared_count != int(manifest["declared_count"]):
                failures.append(
                    f"{identity}: declared count {declared_count} != "
                    f"{manifest['declared_count']}"
                )
            if declared_digest != manifest["declared_digest"]:
                failures.append(
                    f"{identity}: declared digest {declared_digest} != "
                    f"{manifest['declared_digest']}"
                )
            validity = manifest["validity_partition"]
            if validity == "allocated-valid":
                materialization_status = (
                    "materialized-execution-ready"
                    if not obligations
                    else "materialized-typed-obligations"
                )
                semantic_status = (
                    "pending-official-asl-execution"
                    if not obligations
                    else "blocked-typed-projection-obligations"
                )
            else:
                materialization_status = "materialized-nonexecution-input"
                semantic_status = plan_row["execution_eligibility"]
            output.append(
                {
                    "schema": AGGREGATE_SCHEMA,
                    "aggregate_id": identity,
                    "execution_obligation_id": plan_row[
                        "execution_obligation_id"
                    ],
                    "allocation_status_id": plan_row["allocation_status_id"],
                    "semantic_input_id": plan_row["semantic_input_id"],
                    "decode_partition_id": manifest["decode_partition_id"],
                    "validity_partition": validity,
                    "materialization_status": materialization_status,
                    "official_semantic_status": semantic_status,
                    "input_contract_count": str(contract_count),
                    "raw_contract_id_count": str(raw_id_count),
                    "raw_contract_id_digest": raw_id_digest,
                    "input_materialization_id_count": str(input_id_count),
                    "input_materialization_id_digest": input_id_digest,
                    "representation_status": (
                        "representable" if not obligations else "unrepresentable"
                    ),
                    "representation_obligations": (
                        "+".join(obligations) or "none"
                    ),
                    "declared_count": str(declared_count),
                    "declared_digest": declared_digest,
                    "observed_digest": manifest["observed_digest"],
                    "mapping_status": plan_row["mapping_status"],
                    "allocation_behavior_complete": "0",
                }
            )
        if failures:
            raise ValueError(
                f"materializations disagree with aggregate manifest "
                f"({len(failures)}): " + "; ".join(failures[:3])
            )
        if require_execution_ready:
            blocked = [
                row
                for row in output
                if row["validity_partition"] == "allocated-valid"
                and row["representation_status"] != "representable"
            ]
            if blocked:
                raise ValueError(
                    f"{len(blocked)} allocated-valid aggregates retain typed "
                    f"projection obligations; first="
                    + ",".join(row["aggregate_id"] for row in blocked[:3])
                )
        if deferred_checkpoints is not None:
            prune_contribution_checkpoints(
                checkpoint_directory, checkpoints, total_checkpoint_cap
            )
            deferred_checkpoints.extend(checkpoints)
        elif contract_index_output is not None:
            prune_contribution_checkpoints(
                checkpoint_directory, checkpoints, total_checkpoint_cap
            )
            write_contract_manifest(contract_index_output, checkpoints)
        else:
            prune_contribution_checkpoints(
                checkpoint_directory, checkpoints, total_checkpoint_cap
            )
    finally:
        if temporary_checkpoints is not None:
            temporary_checkpoints.cleanup()
    return output


def write_tsv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = _unique_staging_path(path)
    try:
        with temporary.open("w", encoding="utf-8", newline="") as output:
            writer = csv.DictWriter(
                output,
                fieldnames=INDEX_COLUMNS,
                delimiter="\t",
                lineterminator="\n",
            )
            writer.writeheader()
            writer.writerows(rows)
        verify_index_tsv(temporary, rows)
        os.replace(temporary, path)
        verify_index_tsv(path, rows)
    finally:
        if temporary.exists():
            temporary.unlink()


def index_tsv_payload(rows: list[dict[str, str]]) -> bytes:
    text = io.StringIO(newline="")
    writer = csv.DictWriter(
        text, fieldnames=INDEX_COLUMNS, delimiter="\t", lineterminator="\n"
    )
    writer.writeheader()
    writer.writerows(rows)
    return text.getvalue().encode("utf-8")


def verify_index_tsv(path: Path, expected: list[dict[str, str]]) -> None:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"materialization index is not a regular file: {path}")
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != INDEX_COLUMNS:
            raise ValueError(f"{path}: materialization index columns differ")
        actual = list(reader)
    if actual != expected:
        raise ValueError(f"{path}: materialization index contents differ")


def run(options: argparse.Namespace) -> int:
    repository = Path(__file__).resolve().parents[3]
    validate_projection_inventory(options.projection_inventory)
    callback_registry = read_callback_contract_registry(
        options.callback_contracts, repository
    )
    validate_callback_registry_source_coverage(
        callback_registry, repository / "tests/unit/arm64"
    )
    paths = materialization_paths(options.evidence_directory, options.input_jsonl)
    contract_index_output = options.contract_index_output
    if contract_index_output is None:
        contract_index_output = options.output.with_name(
            options.output.stem + ".contracts.tsv.gz"
        )
    checkpoint_directory = options.checkpoint_directory
    if checkpoint_directory is None:
        checkpoint_directory = contract_index_output.with_name(
            contract_index_output.name + ".d"
        )
    read_input_paths = (
        options.aggregate_manifest,
        options.required_variants,
        options.decode_partitions,
        options.projection_inventory,
        options.callback_contracts,
        *paths,
    )
    options.output, contract_index_output, checkpoint_directory = (
        validate_join_output_paths(
            options.output,
            contract_index_output,
            checkpoint_directory,
            read_input_paths,
        )
    )
    with __import__("contextlib").nullcontext(
        (options.output, contract_index_output, checkpoint_directory)
    ) as locked_targets:
        options.output, contract_index_output, checkpoint_directory = locked_targets
        aggregates = read_tsv(options.aggregate_manifest, AGGREGATE_COLUMNS)
        validate_checked_manifest_rows(aggregates)
        required = read_tsv(options.required_variants, REQUIRED_COLUMNS)
        decodes = read_tsv(options.decode_partitions, DECODE_COLUMNS)
        checkpoints: list[CheckpointInfo] = []
        rows = join_materializations(
            paths,
            aggregates,
            required,
            decodes,
            callback_registry,
            guest_page_size=options.guest_page_size,
            allow_partial=options.allow_partial,
            require_execution_ready=options.require_execution_ready,
            contract_index_output=contract_index_output,
            checkpoint_directory=checkpoint_directory,
            free_space_reserve_bytes=reserve_bytes_from_gib(
                options.free_space_reserve_gib
            ),
            spool_bytes_per_contribution=options.spool_bytes_per_contribution,
            checkpoint_bytes_per_contribution=(
                options.checkpoint_bytes_per_contribution
            ),
            deferred_checkpoints=checkpoints,
        )
        for checkpoint in checkpoints:
            if verify_contribution_checkpoint(checkpoint.path) != checkpoint:
                raise ValueError(
                    f"{checkpoint.path}: checkpoint changed before publication"
                )
        outputs = (
            (options.output, index_tsv_payload(rows)),
            (
                contract_index_output,
                contract_manifest_payload(contract_index_output, checkpoints),
            ),
        )

        def validate_staged(staged: list[tuple[Path, Path]]) -> None:
            by_target = {target: temporary for target, temporary in staged}
            verify_index_tsv(by_target[options.output], rows)
            verify_contract_manifest(
                by_target[contract_index_output],
                checkpoints,
                compressed=contract_index_output.suffix == ".gz",
            )

        def recheck_before_replace() -> None:
            for checkpoint in checkpoints:
                if verify_contribution_checkpoint(checkpoint.path) != checkpoint:
                    raise ValueError(
                        f"{checkpoint.path}: checkpoint changed before replacement"
                    )

        def validate_published() -> None:
            verify_index_tsv(options.output, rows)
            verify_contract_manifest(contract_index_output, checkpoints)

        atomic_publish_bytes(
            outputs,
            validate_staged=validate_staged,
            before_replace=recheck_before_replace,
            validate_published=validate_published,
        )
    allocated = sum(
        row["validity_partition"] == "allocated-valid" for row in rows
    )
    ready = sum(
        row["official_semantic_status"] == "pending-official-asl-execution"
        for row in rows
    )
    print(
        f"A64 concrete input join: {len(rows)} aggregate identities, "
        f"{allocated} allocated-valid, {ready} execution-ready, "
        f"0 behavior-complete; wrote {options.output} and "
        f"{contract_index_output} ({len(paths)} verified producer shards in "
        f"{checkpoint_directory})"
    )
    return 0


def main() -> int:
    options = arguments()
    options.evidence_directory = [
        canonical_existing_directory(path, "raw evidence path")
        for path in options.evidence_directory
    ]
    return run(options)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
