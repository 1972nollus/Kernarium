#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build resumable, authority-filtered ASLRef campaign shards."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import gzip
import hashlib
import math
import os
import re
import sqlite3
import tempfile
import uuid
from pathlib import Path
from typing import Callable, Iterable, Mapping, Sequence

from a64_input_materialization import (
    CONTRACT_SCHEMA,
    MEMORY_BLOB_SCHEMA,
    PROGRAM_BLOB_SCHEMA,
    STATE_BLOB_SCHEMA,
    contract_payload,
    iter_raw_materializations,
    raw_contract_id,
    representation_obligations,
    wire_json,
)
from join_a64_input_materializations import (
    CONTRACT_MANIFEST_COLUMNS,
    CONTRACT_MANIFEST_SCHEMA,
    materialization_paths,
)
from a64_raw_evidence_files import (
    canonical_existing_directory,
    require_no_symlink_components,
)
import run_a64_aslref_campaign as campaign


SHARD_SET_SCHEMA = "uml.a64.aslref-campaign-shard-set/v3"
SHARD_FILE = re.compile(r"shard-[0-9]{8}\.inputs\.jsonl\.gz\Z")
STAGE_FILE = re.compile(
    r"shard-[0-9]{8}\.[0-9a-f]{32}\.inputs\.jsonl\.gz\.partial\Z"
)
DEFAULT_MAXIMUM_SHARD_BYTES = 512 * 1024 * 1024
DEFAULT_MAXIMUM_METADATA_BYTES = 128 * 1024 * 1024
DEFAULT_MAXIMUM_DATABASE_BYTES = 128 * 1024 * 1024 * 1024
DEFAULT_MAXIMUM_MERGE_SCRATCH_BYTES = 16 * 1024 * 1024 * 1024
SHARD_METADATA_NAMES = {
    "campaign.lock",
    "shard-set.json",
    "shard-input-list.txt",
}


def add_arguments(parser: argparse.ArgumentParser) -> None:
    repository = Path(__file__).resolve().parents[3]
    parser.add_argument(
        "--evidence-directory", action="append", type=Path, default=[]
    )
    parser.add_argument(
        "--input-jsonl", action="append", type=Path, default=[]
    )
    parser.add_argument(
        "--guest-page-size", type=int, choices=(4096, 16384), required=True
    )
    parser.add_argument(
        "--aggregate-manifest",
        type=Path,
        default=repository
        / "tests/unit/arm64/a64_instruction_aggregate_manifest.tsv",
    )
    parser.add_argument("--materialization-index", type=Path, required=True)
    parser.add_argument(
        "--contract-manifest", action="append", type=Path, required=True
    )
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--local-temp-root", type=Path, required=True)
    parser.add_argument("--expected-total-contracts", type=int)
    parser.add_argument("--minimum-output-free-gib", type=float, default=20.0)
    parser.add_argument("--minimum-local-free-gib", type=float, default=5.0)
    parser.add_argument(
        "--maximum-shard-bytes",
        type=int,
        default=DEFAULT_MAXIMUM_SHARD_BYTES,
    )
    parser.add_argument(
        "--maximum-metadata-bytes",
        type=int,
        default=DEFAULT_MAXIMUM_METADATA_BYTES,
    )
    parser.add_argument(
        "--maximum-database-bytes",
        type=int,
        default=DEFAULT_MAXIMUM_DATABASE_BYTES,
    )
    parser.add_argument(
        "--maximum-merge-scratch-bytes",
        type=int,
        default=DEFAULT_MAXIMUM_MERGE_SCRATCH_BYTES,
    )


def _canonical_output_directory(path: Path) -> Path:
    if not path.is_absolute():
        raise ValueError("campaign shard output directory must be absolute")
    cursor = Path(path.anchor)
    for part in path.parent.parts[1:]:
        cursor = cursor / part
        if cursor.is_symlink():
            raise ValueError(
                "campaign shard output parent contains a symlink: "
                f"{cursor}"
            )
    if not path.parent.is_dir():
        raise ValueError("campaign shard output parent does not exist")
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        raise ValueError("campaign shard output is not a nonsymlink directory")
    if not path.exists():
        path.mkdir()
    return campaign.require_root(path, "campaign shard output")


def _canonical_sharding_roots(
    options: argparse.Namespace,
) -> tuple[Path, Path, tuple[Path, ...]]:
    local_root = campaign.require_root(
        options.local_temp_root, "campaign shard local temp root"
    )
    output_candidate = options.output_directory
    if not output_candidate.is_absolute():
        raise ValueError("campaign shard output directory must be absolute")
    output_candidate = require_no_symlink_components(
        output_candidate, "campaign shard output"
    )
    _require_line_safe_path(output_candidate)

    evidence_directories = tuple(
        canonical_existing_directory(path, "campaign shard evidence directory")
        for path in options.evidence_directory
    )
    explicit_inputs = tuple(
        require_no_symlink_components(path, "campaign shard explicit input")
        for path in options.input_jsonl
    )
    source_directories = tuple(
        sorted(
            {*evidence_directories, *(path.parent for path in explicit_inputs)},
            key=str,
        )
    )

    tooling_root = Path(__file__).resolve().parent
    protected_directories = (*source_directories, tooling_root)
    protected_files = (
        *explicit_inputs,
        options.aggregate_manifest.resolve(strict=False),
        options.materialization_index.resolve(strict=False),
        *(path.resolve(strict=False) for path in options.contract_manifest),
    )
    for root, label in (
        (output_candidate, "campaign shard output"),
        (local_root, "campaign shard local root"),
    ):
        if any(
            campaign._path_contains(root, directory)
            or campaign._path_contains(directory, root)
            for directory in protected_directories
        ):
            raise ValueError(f"{label} aliases a protected source tree")
        if any(
            campaign._path_contains(root, path)
            or campaign._path_contains(path, root)
            for path in protected_files
        ):
            raise ValueError(f"{label} aliases an authoritative input")
    if campaign._path_contains(
        output_candidate, local_root
    ) or campaign._path_contains(local_root, output_candidate):
        raise ValueError("campaign shard output/local roots overlap")
    return output_candidate, local_root, evidence_directories


def _preflight_output_layout(root: Path) -> None:
    metadata = SHARD_METADATA_NAMES
    for path in root.iterdir():
        if path.name in metadata:
            if path.is_symlink() or not path.is_file():
                raise ValueError(
                    f"unsafe campaign shard metadata target: {path.name}"
                )
            continue
        if path.name == ".staging":
            if path.is_symlink() or not path.is_dir():
                raise ValueError("campaign shard staging is not a directory")
            for entry in path.iterdir():
                if (
                    entry.is_symlink()
                    or not entry.is_file()
                    or STAGE_FILE.fullmatch(entry.name) is None
                ):
                    raise ValueError(
                        f"unsafe campaign shard stage entry: {entry.name}"
                    )
            continue
        if SHARD_FILE.fullmatch(path.name) is not None:
            if path.is_symlink() or not path.is_file():
                raise ValueError(
                    f"unsafe campaign shard publication: {path.name}"
                )
            continue
        raise ValueError(f"campaign shard output is not dedicated: {path.name}")


def _database(
    path: Path,
    maximum_bytes: int = DEFAULT_MAXIMUM_DATABASE_BYTES,
) -> sqlite3.Connection:
    campaign._strict_integer(maximum_bytes, "sharding database byte cap", 4096)
    database = sqlite3.connect(path)
    database.execute("PRAGMA journal_mode=OFF")
    database.execute("PRAGMA synchronous=OFF")
    campaign.configure_bounded_sqlite_storage(database, path, maximum_bytes)
    database.execute(
        "CREATE TABLE contract_id(identifier TEXT PRIMARY KEY, "
        "shard_id TEXT NOT NULL, raw_id TEXT NOT NULL)"
    )
    return database


def _tree_file_bytes(root: Path | None) -> int:
    if root is None or not root.exists():
        return 0
    return sum(path.stat().st_size for path in root.rglob("*") if path.is_file())


def _require_metadata_cap(
    root: Path,
    maximum_bytes: int,
    replacements: Mapping[str, bytes] | None = None,
) -> None:
    maximum_bytes = campaign._strict_integer(
        maximum_bytes, "campaign shard metadata cap", 1
    )
    replacements = replacements or {}
    if not set(replacements) <= SHARD_METADATA_NAMES:
        raise ValueError("campaign shard metadata replacement differs")
    total = 0
    for name in SHARD_METADATA_NAMES:
        if name in replacements:
            total += len(replacements[name])
            continue
        path = root / name
        if path.exists():
            if path.is_symlink() or not path.is_file():
                raise ValueError(f"unsafe campaign shard metadata target: {name}")
            total += path.stat().st_size
    if total > maximum_bytes:
        raise ValueError("campaign shard metadata exceeded its hard byte cap")


def _require_published_caps(
    root: Path, options: argparse.Namespace
) -> None:
    maximum_shard_bytes = campaign._strict_integer(
        options.maximum_shard_bytes, "campaign shard byte cap", 1
    )
    _require_metadata_cap(root, options.maximum_metadata_bytes)
    for path in root.iterdir():
        if (
            SHARD_FILE.fullmatch(path.name) is not None
            and path.stat().st_size > maximum_shard_bytes
        ):
            raise ValueError("published campaign shard exceeds its hard byte cap")


def _require_sharding_capacity(
    options: argparse.Namespace,
    output_root: Path,
    local_root: Path,
    work_root: Path | None,
    staging: Path | None,
) -> None:
    maximum_shard_bytes = campaign._strict_integer(
        options.maximum_shard_bytes, "campaign shard byte cap", 1
    )
    maximum_metadata_bytes = campaign._strict_integer(
        options.maximum_metadata_bytes, "campaign shard metadata cap", 1
    )
    maximum_database_bytes = campaign._strict_integer(
        options.maximum_database_bytes, "campaign shard database cap", 4096
    )
    maximum_scratch_bytes = campaign._strict_integer(
        options.maximum_merge_scratch_bytes,
        "campaign shard merge scratch cap",
        1,
    )
    staged_bytes = _tree_file_bytes(staging)
    if staged_bytes > maximum_shard_bytes:
        raise ValueError("campaign shard stage exceeded its hard byte cap")
    local_bytes = _tree_file_bytes(work_root)
    maximum_local_bytes = maximum_database_bytes + maximum_scratch_bytes
    if local_bytes > maximum_local_bytes:
        raise ValueError("campaign shard local work exceeded its hard byte cap")
    campaign.require_free_space(
        output_root,
        options.minimum_output_free_gib,
        "shard output",
        headroom_bytes=(
            maximum_metadata_bytes + maximum_shard_bytes - staged_bytes
        ),
    )
    campaign.require_free_space(
        local_root,
        options.minimum_local_free_gib,
        "shard local",
        headroom_bytes=maximum_local_bytes - local_bytes,
    )


def _load_authority(
    options: argparse.Namespace,
    database: sqlite3.Connection,
    output_root: Path | None = None,
    local_root: Path | None = None,
    work_root: Path | None = None,
    staging: Path | None = None,
) -> Mapping[str, object]:
    output_root = output_root or options.output_directory
    local_root = local_root or options.local_temp_root

    def progress(_contribution_count: int) -> None:
        _require_sharding_capacity(
            options, output_root, local_root, work_root, staging
        )

    return campaign.load_authoritative_input(
        guest_page_size=options.guest_page_size,
        aggregate_manifest=options.aggregate_manifest.resolve(),
        materialization_index=options.materialization_index.resolve(),
        contract_manifests=[
            path.resolve() for path in options.contract_manifest
        ],
        database=database,
        require_campaign_set=False,
        progress_callback=progress,
        maximum_merge_scratch_bytes=options.maximum_merge_scratch_bytes,
        before_merge_scratch_write=lambda _bytes: _require_sharding_capacity(
            options, output_root, local_root, work_root, staging
        ),
    )


def _source_identities(
    input_paths: Sequence[Path], contract_manifests: Sequence[Path]
) -> list[dict[str, object]]:
    expected: dict[str, tuple[int, str]] = {}
    for manifest in sorted(
        campaign.require_regular_file(path, "contract manifest")
        for path in contract_manifests
    ):
        for row in campaign.read_tsv(manifest, CONTRACT_MANIFEST_COLUMNS):
            if row["schema"] != CONTRACT_MANIFEST_SCHEMA:
                raise ValueError("contract manifest schema differs")
            name = row["source_name"]
            if Path(name).name != name or name in expected:
                raise ValueError(
                    "authoritative contract source names are not unique basenames"
                )
            encoded_size = row["source_size"]
            if re.fullmatch(r"[1-9][0-9]*", encoded_size) is None:
                raise ValueError("contract source size is invalid")
            size = int(encoded_size)
            if (
                size <= 0
                or campaign.SHA256.fullmatch(row["source_sha256"]) is None
            ):
                raise ValueError("contract source identity is invalid")
            expected[name] = (size, row["source_sha256"])
    observed: dict[str, dict[str, object]] = {}
    for path in input_paths:
        path = campaign.require_regular_file(path, "materialization source")
        if path.name in observed:
            raise ValueError("materialization source basenames repeat")
        identity = campaign.file_identity(path)
        expected_identity = expected.get(path.name)
        if expected_identity is None or expected_identity != (
            identity["size"], identity["sha256"]
        ):
            raise ValueError(
                f"materialization source identity differs: {path.name}"
            )
        observed[path.name] = {
            "name": path.name,
            "path": identity["path"],
        }
    if set(observed) != set(expected):
        raise ValueError(
            "materialization sources differ from authoritative producers"
        )
    return [observed[name] for name in sorted(observed)]


def _event(schema: str, identifier: str, payload: object) -> dict[str, object]:
    return {"schema": schema, "id": identifier, "payload": payload}


class _CappedShardOutput:
    def __init__(
        self,
        raw: object,
        maximum_bytes: int,
        before_write: Callable[[int], None] | None,
    ) -> None:
        self.raw = raw
        self.maximum_bytes = campaign._strict_integer(
            maximum_bytes, "campaign shard byte cap", 1
        )
        self.before_write = before_write
        self.written = 0

    def write(self, payload: bytes) -> int:
        if self.before_write is not None:
            self.before_write(len(payload))
        if self.written + len(payload) > self.maximum_bytes:
            raise ValueError("campaign shard output exceeded its hard byte cap")
        written = self.raw.write(payload)  # type: ignore[attr-defined]
        self.written += written
        return written

    def flush(self) -> None:
        self.raw.flush()  # type: ignore[attr-defined]

    def tell(self) -> int:
        return int(self.raw.tell())  # type: ignore[attr-defined]


class ShardWriter:
    def __init__(
        self,
        path: Path,
        shard_id: str,
        maximum_bytes: int = DEFAULT_MAXIMUM_SHARD_BYTES,
        before_write: Callable[[int], None] | None = None,
    ) -> None:
        self.path = path
        self.shard_id = shard_id
        self.count = 0
        self.identifier_digest = hashlib.sha256()
        self.emitted_blobs: set[str] = set()
        self.closed = False
        self.raw = path.open("xb")
        self.capped = _CappedShardOutput(
            self.raw, maximum_bytes, before_write
        )
        self.compressed = gzip.GzipFile(mode="wb", fileobj=self.capped)

    def _write(self, value: Mapping[str, object]) -> None:
        self.compressed.write(wire_json(value).encode("utf-8") + b"\n")

    def add(self, record: Mapping[str, object]) -> None:
        if self.count >= campaign.MAX_SHARD_CONTRACTS:
            raise ValueError("campaign shard writer exceeded its hard cap")
        for schema, identifier_name, payload_name in (
            (PROGRAM_BLOB_SCHEMA, "program_id", "program"),
            (STATE_BLOB_SCHEMA, "initial_state_id", "initial_state"),
            (STATE_BLOB_SCHEMA, "expected_state_id", "expected_state"),
            (MEMORY_BLOB_SCHEMA, "memory_id", "memory_regions"),
        ):
            identifier = str(record[identifier_name])
            if identifier in self.emitted_blobs:
                continue
            self._write(_event(schema, identifier, record[payload_name]))
            self.emitted_blobs.add(identifier)
        identifier = str(record["input_contract_id"])
        self._write(
            {
                "schema": CONTRACT_SCHEMA,
                "id": identifier,
                "payload": contract_payload(record),
                "raw_occurrences": "0000000000000001",
            }
        )
        encoded = identifier.encode("ascii")
        self.identifier_digest.update(len(encoded).to_bytes(8, "big"))
        self.identifier_digest.update(encoded)
        self.count += 1

    def finish(self) -> tuple[int, str]:
        if self.closed:
            raise ValueError("campaign shard writer is already closed")
        try:
            self.compressed.close()
        finally:
            self.raw.close()
            self.closed = True
        if self.count == 0:
            raise ValueError("campaign shard writer produced an empty shard")
        return self.count, self.identifier_digest.hexdigest()

    def abort(self) -> None:
        if not self.closed:
            try:
                self.compressed.close()
            except (OSError, ValueError):
                pass
            finally:
                self.raw.close()
                self.closed = True
        if self.path.exists():
            self.path.unlink()


def _validated_record(
    record: Mapping[str, object], guest_page_size: int
) -> tuple[str, str]:
    obligations = representation_obligations(record)
    if obligations:
        raise ValueError(
            f"{record['input_contract_id']}: representation obligations: "
            + "+".join(obligations)
        )
    page = int(str(record["program"]["guest_page_size"]), 16)  # type: ignore[index]
    if page != guest_page_size:
        raise ValueError("materialization source mixes guest page profiles")
    engines = record["engine_results"]
    if not isinstance(engines, Mapping) or (
        engines.get("interpreter")
        not in {"executed-not-retired", "executed-retired"}
        or engines.get("gadgets")
        not in {"executed-not-retired", "executed-retired"}
        or engines.get("jit")
        not in {
            "executed-not-retired",
            "executed-retired",
            "skipped-host-feature",
        }
    ):
        raise ValueError("materialization source lacks exact engine evidence")
    identifier = str(record["input_contract_id"])
    if campaign.INPUT_ID.fullmatch(identifier) is None:
        raise ValueError("materialization input ID is invalid")
    return identifier, raw_contract_id(record)


def _iter_distinct_source_materializations(
    input_paths: Sequence[Path],
    database: sqlite3.Connection,
    guest_page_size: int,
) -> Iterable[tuple[Mapping[str, object], str, str]]:
    """Reconstruct one producer at a time and deduplicate globally on disk.

    This bounds Python reconstruction state by the largest single producer,
    rather than the full page-profile corpus.  The temporary SQLite table
    preserves exact cross-producer duplicate semantics without a global
    in-memory set.
    """
    database.execute(
        "CREATE TABLE source_contract_seen("
        "identifier TEXT PRIMARY KEY, raw_id TEXT NOT NULL) WITHOUT ROWID"
    )
    for path in sorted(input_paths):
        for record in iter_raw_materializations([path]):
            identifier, raw_id = _validated_record(record, guest_page_size)
            authoritative = database.execute(
                "SELECT 1 FROM authoritative_contract_id WHERE identifier = ?",
                (identifier,),
            ).fetchone()
            if authoritative is None:
                continue
            try:
                database.execute(
                    "INSERT INTO source_contract_seen VALUES (?, ?)",
                    (identifier, raw_id),
                )
            except sqlite3.IntegrityError:
                previous = database.execute(
                    "SELECT raw_id FROM source_contract_seen "
                    "WHERE identifier = ?",
                    (identifier,),
                ).fetchone()
                if previous is None or previous[0] != raw_id:
                    raise ValueError(
                        f"{identifier}: duplicate materialization has a "
                        "different raw semantic binding"
                    )
                continue
            yield record, identifier, raw_id


def _publish_generated_shard(
    writer: ShardWriter,
    root: Path,
    staging: Path,
    guest_page_size: int,
) -> dict[str, object]:
    count, identifier_digest = writer.finish()
    stage = writer.path
    final = root / f"{writer.shard_id}.inputs.jsonl.gz"
    staged_count, staged_digest = campaign.scan_shard_input(
        stage, guest_page_size=guest_page_size
    )
    if staged_count != count or staged_digest != identifier_digest:
        raise ValueError(f"generated campaign shard differs: {final.name}")
    if final.exists():
        if final.is_symlink() or not final.is_file():
            raise ValueError(f"existing campaign shard is unsafe: {final.name}")
        existing_count, existing_digest = campaign.scan_shard_input(
            final, guest_page_size=guest_page_size
        )
        if existing_count != count or existing_digest != identifier_digest:
            raise ValueError(
                f"existing campaign shard differs: {final.name}"
            )
        stage.unlink()
    else:
        os.replace(stage, final)
    observed_count, observed_digest = campaign.scan_shard_input(
        final, guest_page_size=guest_page_size
    )
    if observed_count != count or observed_digest != identifier_digest:
        raise ValueError(f"generated campaign shard differs: {final.name}")
    if any(staging.iterdir()):
        raise ValueError("campaign shard staging directory is not empty")
    return {
        "shard_id": writer.shard_id,
        "path": str(final),
        "contract_count": count,
        "input_contract_ids_sha256": identifier_digest,
    }


def _manifest_payload(
    *,
    page: int,
    authority: Mapping[str, object],
    sources: Sequence[Mapping[str, object]],
    shards: Sequence[Mapping[str, object]],
) -> dict[str, object]:
    return {
        "schema": SHARD_SET_SCHEMA,
        "guest_page_size": page,
        "shard_contract_cap": campaign.MAX_SHARD_CONTRACTS,
        "total_contracts": sum(int(row["contract_count"]) for row in shards),
        "authoritative_input": dict(authority),
        "source_inputs": list(sources),
        "shards": list(shards),
    }


def _input_list_payload(shards: Sequence[Mapping[str, object]]) -> bytes:
    return "".join(f"{row['path']}\n" for row in shards).encode("utf-8")


def _require_line_safe_path(path: Path) -> None:
    if any(ord(character) < 0x20 or ord(character) == 0x7F for character in str(path)):
        raise ValueError("campaign shard output path contains a control character")


def _exact_json_equal(left: object, right: object) -> bool:
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return set(left) == set(right) and all(  # type: ignore[arg-type]
            _exact_json_equal(left[key], right[key])  # type: ignore[index]
            for key in left
        )
    if isinstance(left, list):
        return len(left) == len(right) and all(  # type: ignore[arg-type]
            _exact_json_equal(a, b)
            for a, b in zip(left, right, strict=True)  # type: ignore[arg-type]
        )
    return left == right


def _verify_shard_set(
    root: Path,
    manifest: Mapping[str, object],
    authority: Mapping[str, object],
    sources: Sequence[Mapping[str, object]],
    database: sqlite3.Connection,
) -> Mapping[str, object]:
    expected_keys = {
        "schema",
        "guest_page_size",
        "shard_contract_cap",
        "total_contracts",
        "authoritative_input",
        "source_inputs",
        "shards",
    }
    if set(manifest) != expected_keys or manifest["schema"] != SHARD_SET_SCHEMA:
        raise ValueError("campaign shard-set manifest schema differs")
    page = campaign._strict_integer(
        manifest["guest_page_size"],
        "campaign shard-set guest page size",
        4096,
        16384,
    )
    cap = campaign._strict_integer(
        manifest["shard_contract_cap"],
        "campaign shard-set contract cap",
        campaign.MAX_SHARD_CONTRACTS,
        campaign.MAX_SHARD_CONTRACTS,
    )
    if (
        page not in {4096, 16384}
        or cap != campaign.MAX_SHARD_CONTRACTS
        or not _exact_json_equal(manifest["authoritative_input"], authority)
        or not _exact_json_equal(manifest["source_inputs"], list(sources))
    ):
        raise ValueError("campaign shard-set binding differs")
    shards = manifest["shards"]
    if not isinstance(shards, list) or not shards:
        raise ValueError("campaign shard set is empty")
    database.execute("DELETE FROM contract_id")
    total = 0
    expected_files = set()
    for index, row in enumerate(shards):
        shard_id = f"shard-{index:08d}"
        expected = {
            "shard_id",
            "path",
            "contract_count",
            "input_contract_ids_sha256",
        }
        if not isinstance(row, dict) or set(row) != expected:
            raise ValueError("campaign shard-set row differs")
        path = root / f"{shard_id}.inputs.jsonl.gz"
        if row["shard_id"] != shard_id or row["path"] != str(path):
            raise ValueError("campaign shard-set path/ID differs")
        campaign.require_regular_file(path, "campaign shard")
        count = campaign._strict_integer(
            row["contract_count"],
            "campaign shard-set contract count",
            1,
            campaign.MAX_SHARD_CONTRACTS,
        )
        if index + 1 != len(shards) and count != campaign.MAX_SHARD_CONTRACTS:
            raise ValueError("nonfinal campaign shard is not full")
        if (
            not isinstance(row["input_contract_ids_sha256"], str)
            or campaign.SHA256.fullmatch(
                str(row["input_contract_ids_sha256"])
            )
            is None
        ):
            raise ValueError("campaign shard-set contract digest differs")
        observed_count, observed_digest = campaign.scan_shard_input(
            path,
            database,
            shard_id,
            page,
        )
        if (
            observed_count != count
            or observed_digest != row["input_contract_ids_sha256"]
        ):
            raise ValueError("campaign shard-set contract identity differs")
        total += count
        expected_files.add(path.name)
    campaign.require_exact_authoritative_id_set(database)
    campaign.require_exact_authoritative_raw_bindings(database)
    declared_total = campaign._strict_integer(
        manifest["total_contracts"], "campaign shard-set total", 1
    )
    if (
        declared_total != total
        or total != authority["input_contract_count"]
    ):
        raise ValueError("campaign shard-set total differs from authority")
    expected_root = {
        "campaign.lock",
        "shard-set.json",
        "shard-input-list.txt",
        ".staging",
        *expected_files,
    }
    if {path.name for path in root.iterdir()} != expected_root:
        raise ValueError("campaign shard output contains unrelated entries")
    staging = root / ".staging"
    if staging.is_symlink() or not staging.is_dir() or any(staging.iterdir()):
        raise ValueError("campaign shard staging is not empty")
    expected_list = _input_list_payload(shards)
    if (root / "shard-input-list.txt").read_bytes() != expected_list:
        raise ValueError("campaign shard input list differs")
    expected_manifest = (campaign.canonical_json(manifest) + "\n").encode(
        "utf-8"
    )
    if (root / "shard-set.json").read_bytes() != expected_manifest:
        raise ValueError("campaign shard-set publication differs")
    return manifest


def _write_or_compare(path: Path, payload: bytes) -> None:
    if path.exists():
        if path.is_symlink() or not path.is_file() or path.read_bytes() != payload:
            raise ValueError(f"existing shard metadata differs: {path.name}")
        return
    campaign.atomic_write(path, payload)


def _prepare_root(
    root: Path,
    maximum_metadata_bytes: int = DEFAULT_MAXIMUM_METADATA_BYTES,
) -> Path:
    allowed = {
        "campaign.lock",
        "shard-set.json",
        "shard-input-list.txt",
        ".staging",
    }
    unrelated = [
        path.name
        for path in root.iterdir()
        if path.name not in allowed and SHARD_FILE.fullmatch(path.name) is None
    ]
    if unrelated:
        raise ValueError(
            "campaign shard output is not dedicated: "
            + ",".join(sorted(unrelated)[:3])
        )
    staging = root / ".staging"
    staging.mkdir(exist_ok=True)
    if staging.is_symlink() or not staging.is_dir():
        raise ValueError("campaign shard staging is not a directory")
    for path in list(staging.iterdir()):
        if path.is_symlink() or not path.is_file() or STAGE_FILE.fullmatch(path.name) is None:
            raise ValueError(f"unsafe campaign shard stage entry: {path.name}")
        path.unlink()
    _require_metadata_cap(root, maximum_metadata_bytes, {})
    return staging


def _generate(
    options: argparse.Namespace,
    root: Path,
    staging: Path,
    input_paths: Sequence[Path],
    authority: Mapping[str, object],
    sources: Sequence[Mapping[str, object]],
    database: sqlite3.Connection,
    local_root: Path,
    work_root: Path,
) -> Mapping[str, object]:
    shards: list[dict[str, object]] = []
    writer: ShardWriter | None = None
    scanned = 0

    def start_writer() -> ShardWriter:
        shard_id = f"shard-{len(shards):08d}"
        stage = staging / (
            f"{shard_id}.{uuid.uuid4().hex}.inputs.jsonl.gz.partial"
        )
        _require_sharding_capacity(
            options, root, local_root, work_root, staging
        )
        return ShardWriter(
            stage,
            shard_id,
            options.maximum_shard_bytes,
            lambda _bytes: _require_sharding_capacity(
                options, root, local_root, work_root, staging
            ),
        )

    try:
        for record, identifier, raw_id in _iter_distinct_source_materializations(
            input_paths, database, options.guest_page_size
        ):
            scanned += 1
            if scanned % campaign.MAX_SHARD_CONTRACTS == 0:
                _require_sharding_capacity(
                    options, root, local_root, work_root, staging
                )
            expected_raw = database.execute(
                "SELECT MIN(raw_id), MAX(raw_id) FROM authoritative_binding "
                "WHERE input_id = ?",
                (identifier,),
            ).fetchone()
            if expected_raw is None or expected_raw[0] is None:
                continue
            if expected_raw[0] != expected_raw[1] or expected_raw[0] != raw_id:
                raise ValueError(
                    f"{identifier}: authoritative raw semantic binding differs"
                )
            if writer is None:
                writer = start_writer()
            database.execute(
                "INSERT INTO contract_id VALUES (?, ?, ?)",
                (identifier, writer.shard_id, raw_id),
            )
            writer.add(record)
            if writer.count == campaign.MAX_SHARD_CONTRACTS:
                row = _publish_generated_shard(
                    writer,
                    root,
                    staging,
                    options.guest_page_size,
                )
                shards.append(row)
                writer = None
        if writer is not None:
            row = _publish_generated_shard(
                writer,
                root,
                staging,
                options.guest_page_size,
            )
            shards.append(row)
            writer = None
    finally:
        if writer is not None:
            writer.abort()
    if not shards:
        raise ValueError("no authoritative execution-ready inputs were sharded")
    campaign.require_exact_authoritative_id_set(database)
    campaign.require_exact_authoritative_raw_bindings(database)
    database.execute("DROP TABLE source_contract_seen")
    expected_files = {f"{row['shard_id']}.inputs.jsonl.gz" for row in shards}
    actual_files = {
        path.name for path in root.iterdir() if SHARD_FILE.fullmatch(path.name)
    }
    if actual_files != expected_files:
        raise ValueError("campaign shard file set differs after generation")
    manifest = _manifest_payload(
        page=options.guest_page_size,
        authority=authority,
        sources=sources,
        shards=shards,
    )
    if (
        options.expected_total_contracts is not None
        and manifest["total_contracts"] != options.expected_total_contracts
    ):
        raise ValueError("campaign shard total differs from explicit expectation")
    list_payload = _input_list_payload(shards)
    manifest_payload = (campaign.canonical_json(manifest) + "\n").encode(
        "utf-8"
    )
    _require_metadata_cap(
        root,
        options.maximum_metadata_bytes,
        {
            "shard-input-list.txt": list_payload,
            "shard-set.json": manifest_payload,
        },
    )
    _require_sharding_capacity(
        options, root, local_root, work_root, staging
    )
    _write_or_compare(
        root / "shard-input-list.txt",
        list_payload,
    )
    _write_or_compare(
        root / "shard-set.json",
        manifest_payload,
    )
    _require_published_caps(root, options)
    database.execute("DELETE FROM contract_id")
    return _verify_shard_set(
        root,
        manifest,
        authority,
        sources,
        database,
    )


def _run_locked(
    options: argparse.Namespace,
    output_candidate: Path,
    local_root: Path,
    evidence_directories: Sequence[Path],
) -> Mapping[str, object]:
    if (
        isinstance(options.guest_page_size, bool)
        or not isinstance(options.guest_page_size, int)
        or options.guest_page_size not in {4096, 16384}
    ):
        raise ValueError("campaign shard guest page size is invalid")
    if (
        options.expected_total_contracts is not None
        and (
            isinstance(options.expected_total_contracts, bool)
            or not isinstance(options.expected_total_contracts, int)
            or options.expected_total_contracts <= 0
        )
    ):
        raise ValueError("expected campaign contract total is invalid")
    for value, label in (
        (options.minimum_output_free_gib, "shard output free-space reserve"),
        (options.minimum_local_free_gib, "shard local free-space reserve"),
    ):
        if (
            isinstance(value, bool)
            or not isinstance(value, (int, float))
            or not math.isfinite(value)
            or value < 0
        ):
            raise ValueError(f"{label} is invalid")
    for value, label, minimum in (
        (options.maximum_shard_bytes, "shard byte cap", 1),
        (options.maximum_metadata_bytes, "shard metadata cap", 1),
        (options.maximum_database_bytes, "shard database cap", 4096),
        (
            options.maximum_merge_scratch_bytes,
            "shard merge scratch cap",
            1,
        ),
    ):
        campaign._strict_integer(value, label, minimum)
    input_paths = materialization_paths(
        evidence_directories, options.input_jsonl
    )
    sources = _source_identities(input_paths, options.contract_manifest)
    root = _canonical_output_directory(output_candidate)
    _preflight_output_layout(root)
    _require_published_caps(root, options)
    _require_sharding_capacity(
        options, root, local_root, None, None
    )
    with campaign.campaign_lock(root):
        _preflight_output_layout(root)
        _require_published_caps(root, options)
        with tempfile.TemporaryDirectory(
            prefix="a64-campaign-sharding-", dir=local_root
        ) as temporary:
            work_root = Path(temporary)
            database = _database(
                work_root / "authority.sqlite3",
                options.maximum_database_bytes,
            )
            try:
                authority = _load_authority(
                    options,
                    database,
                    root,
                    local_root,
                    work_root,
                    None,
                )
                authority_files = [
                    Path(str(identity["path"]))
                    for key in (
                        "contract_manifests",
                        "checkpoint_files",
                    )
                    for identity in authority[key]  # type: ignore[index]
                ]
                authority_files.extend(
                    Path(str(authority[key]["path"]))  # type: ignore[index]
                    for key in (
                        "aggregate_manifest",
                        "materialization_index",
                    )
                )
                all_input_files = [*input_paths, *authority_files]
                if any(
                    campaign._path_contains(root, path)
                    or campaign._path_contains(path, root)
                    for path in all_input_files
                ):
                    raise ValueError(
                        "campaign shard output aliases authoritative input"
                    )
                if any(
                    campaign._path_contains(local_root, path)
                    or campaign._path_contains(path, local_root)
                    for path in all_input_files
                ):
                    raise ValueError(
                        "campaign shard local root aliases authoritative input"
                    )
                staging = _prepare_root(
                    root,
                    options.maximum_metadata_bytes,
                )
                manifest_path = root / "shard-set.json"
                if manifest_path.exists():
                    manifest = campaign.read_canonical_json(manifest_path)
                    if (
                        options.expected_total_contracts is not None
                        and manifest.get("total_contracts")
                        != options.expected_total_contracts
                    ):
                        raise ValueError(
                            "campaign shard total differs from explicit expectation"
                        )
                    if not (root / "shard-input-list.txt").exists():
                        shards = manifest.get("shards")
                        if not isinstance(shards, list):
                            raise ValueError("partial shard manifest differs")
                        list_payload = _input_list_payload(shards)
                        _require_metadata_cap(
                            root,
                            options.maximum_metadata_bytes,
                            {"shard-input-list.txt": list_payload},
                        )
                        _write_or_compare(
                            root / "shard-input-list.txt",
                            list_payload,
                        )
                    verified = _verify_shard_set(
                        root,
                        manifest,
                        authority,
                        sources,
                        database,
                    )
                    _require_published_caps(root, options)
                    return verified
                return _generate(
                    options,
                    root,
                    staging,
                    input_paths,
                    authority,
                    sources,
                    database,
                    local_root,
                    work_root,
                )
            finally:
                database.close()


def run(options: argparse.Namespace) -> Mapping[str, object]:
    output_candidate, local_root, evidence_directories = (
        _canonical_sharding_roots(options)
    )
    return _run_locked(
        options,
        output_candidate,
        local_root,
        evidence_directories,
    )
