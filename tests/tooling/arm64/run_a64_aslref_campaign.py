#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Plan, run, resume, and verify immutable ASLRef execution shards."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import concurrent.futures
import csv
import errno
import fcntl
import gzip
import hashlib
import heapq
import io
import json
import math
import os
import re
import shutil
import sqlite3
import sys
import sysconfig
import tempfile
import threading
import time
import uuid
from contextlib import contextmanager
from pathlib import Path
from typing import Callable, Iterable, Iterator, Mapping, Sequence

from a64_aggregate_manifest_identity import (
    read_manifest_identity,
    validate_manifest_identity,
)
from a64_bounded_generation import (
    BoundedWorkspace,
    GenerationLimits,
    run_bounded,
)
from a64_input_materialization import (
    iter_raw_materializations,
    raw_contract_id,
    representation_obligations,
)
from a64_raw_evidence_files import path_is_ancestor, paths_alias
from aggregate_a64_raw_instruction_evidence import (
    OUTPUT_COLUMNS as AGGREGATE_COLUMNS,
)
from join_a64_input_materializations import (
    AggregateAccumulator,
    CONTRACT_INDEX_SCHEMA,
    CONTRACT_MANIFEST_COLUMNS,
    CONTRACT_MANIFEST_SCHEMA,
    CONTRIBUTION_CHECKPOINT_FOOTER_SCHEMA,
    CONTRIBUTION_CHECKPOINT_HEADER_SCHEMA,
    INDEX_COLUMNS as MATERIALIZATION_INDEX_COLUMNS,
    verify_contribution_checkpoint,
)


PLAN_SCHEMA = "uml.a64.aslref-shard-campaign-plan/v4"
ARTIFACT_SCHEMA = "uml.a64.aslref-shard-artifact-manifest/v1"
SUCCESS_SCHEMA = "uml.a64.aslref-shard-success/v3"
REPORT_SCHEMA = "uml.a64.aslref-shard-campaign-report/v4"
PILOT_SCHEMA = "uml.a64.aslref-shard-campaign-pilot/v1"
SHARD_ID = re.compile(r"shard-[0-9]{8}\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
INPUT_ID = re.compile(r"input-contract-[0-9a-f]{64}\Z")
PROBE_FORMAT = "uml.a64.aslref-materialization-batch-probe/v1"
MAX_SHARD_CONTRACTS = 1024
DEFAULT_MAX_ENCODED_BYTES = 128 * 1024 * 1024
DEFAULT_MAX_RSS_KIB = 2 * 1024 * 1024
DEFAULT_MAX_ARTIFACT_BYTES = 512 * 1024 * 1024
DEFAULT_MAX_ARTIFACT_FILES = 4096
DEFAULT_MAX_LOG_BYTES = 8 * 1024 * 1024
PROBE_MINIMUM_GENERATED_FREE_GIB = 1.0
DIRECTORY_SYNC_UNSUPPORTED = {errno.EINVAL, errno.ENOTSUP, errno.EOPNOTSUPP}
MAX_ARTIFACT_SCALAR = (1 << 63) - 1
DIRECTORY_SYNC_POLICIES = {"require", "record-unsupported"}
ISOLATED_PYTHON_FLAGS = ("-E", "-s", "-S", "-B")
MAX_CHECKPOINT_MERGE_FAN_IN = 32
DEFAULT_MAX_AUTHORITY_DATABASE_BYTES = 128 * 1024 * 1024 * 1024
DEFAULT_MAX_AUTHORITY_MERGE_SCRATCH_BYTES = 16 * 1024 * 1024 * 1024


def canonical_json(value: object) -> str:
    return json.dumps(
        value,
        ensure_ascii=True,
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    )


def configure_bounded_sqlite_storage(
    database: sqlite3.Connection,
    database_path: Path,
    maximum_database_bytes: int,
) -> None:
    maximum_database_bytes = _strict_integer(
        maximum_database_bytes, "SQLite database byte cap", 4096
    )
    temporary_root = database_path.parent.resolve(strict=True)
    quoted = str(temporary_root).replace("'", "''")
    database.execute("PRAGMA temp_store=FILE")
    database.execute(f"PRAGMA temp_store_directory='{quoted}'")
    observed = database.execute("PRAGMA temp_store_directory").fetchone()
    if (
        observed is None
        or len(observed) != 1
        or Path(str(observed[0])).resolve(strict=True) != temporary_root
    ):
        raise ValueError("SQLite temporary storage escaped its checked root")
    page_size = int(database.execute("PRAGMA page_size").fetchone()[0])
    maximum_pages = maximum_database_bytes // page_size
    installed = int(
        database.execute(f"PRAGMA max_page_count={maximum_pages}").fetchone()[0]
    )
    if installed != maximum_pages:
        raise ValueError("SQLite database hard cap was not installed")


def file_identity(path: Path) -> dict[str, object]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
            size += len(block)
    return {
        "kind": "file",
        "path": str(path.resolve()),
        "size": size,
        "file_count": 1,
        "sha256": digest.hexdigest(),
    }


def tree_identity(path: Path) -> dict[str, object]:
    root = path.resolve()
    digest = hashlib.sha256()
    size = 0
    count = 0
    for candidate in sorted(root.rglob("*"), key=lambda item: item.as_posix()):
        relative = candidate.relative_to(root)
        if "__pycache__" in relative.parts or candidate.name in {".DS_Store"}:
            continue
        if candidate.is_symlink():
            raise ValueError(f"frozen source tree contains symlink {candidate}")
        if not candidate.is_file():
            continue
        encoded = relative.as_posix().encode("utf-8")
        digest.update(len(encoded).to_bytes(8, "big"))
        digest.update(encoded)
        identity = file_identity(candidate)
        digest.update(int(identity["size"]).to_bytes(8, "big"))
        digest.update(bytes.fromhex(str(identity["sha256"])))
        size += int(identity["size"])
        count += 1
    if count == 0:
        raise ValueError(f"frozen source tree is empty: {path}")
    return {
        "kind": "tree",
        "path": str(root),
        "size": size,
        "file_count": count,
        "sha256": digest.hexdigest(),
    }


def python_runtime_identity(path: Path) -> dict[str, object]:
    """Freeze the isolated (-S) stdlib load tree and resolved link files."""
    root = path.resolve(strict=True)
    digest = hashlib.sha256()
    size = 0
    count = 0
    for candidate in sorted(root.rglob("*"), key=lambda item: item.as_posix()):
        relative = candidate.relative_to(root)
        if (
            "__pycache__" in relative.parts
            or candidate.name == ".DS_Store"
            or relative.parts[:1] == ("site-packages",)
        ):
            continue
        encoded = relative.as_posix().encode("utf-8")
        if candidate.is_symlink():
            link = os.readlink(candidate).encode("utf-8")
            resolved = candidate.resolve(strict=True)
            if resolved.is_dir():
                try:
                    resolved.relative_to(root)
                except ValueError as error:
                    raise ValueError(
                        "isolated Python runtime has an external directory "
                        f"symlink: {candidate}"
                    ) from error
                target_kind = b"internal-directory"
                target_size = 0
                target_digest = b""
            elif resolved.is_file():
                identity = file_identity(resolved)
                target_kind = b"file"
                target_size = int(identity["size"])
                target_digest = bytes.fromhex(str(identity["sha256"]))
            else:
                raise ValueError(
                    f"isolated Python runtime link target is unsupported: {candidate}"
                )
            digest.update(b"L")
            digest.update(len(encoded).to_bytes(8, "big"))
            digest.update(encoded)
            digest.update(len(link).to_bytes(8, "big"))
            digest.update(link)
            digest.update(len(target_kind).to_bytes(8, "big"))
            digest.update(target_kind)
            digest.update(target_size.to_bytes(8, "big"))
            digest.update(target_digest)
            size += len(link) + target_size
            count += 1
            continue
        if not candidate.is_file():
            continue
        identity = file_identity(candidate)
        digest.update(b"F")
        digest.update(len(encoded).to_bytes(8, "big"))
        digest.update(encoded)
        digest.update(int(identity["size"]).to_bytes(8, "big"))
        digest.update(bytes.fromhex(str(identity["sha256"])))
        size += int(identity["size"])
        count += 1
    if count == 0:
        raise ValueError(f"isolated Python runtime tree is empty: {path}")
    return {
        "kind": "python-runtime",
        "path": str(root),
        "size": size,
        "file_count": count,
        "sha256": digest.hexdigest(),
    }


def python_library_path() -> Path:
    framework = Path(sys.base_prefix).resolve(strict=True) / "Python"
    if framework.is_file():
        return framework.resolve(strict=True)
    library = sysconfig.get_config_var("LDLIBRARY")
    directory = sysconfig.get_config_var("LIBDIR")
    if isinstance(library, str) and isinstance(directory, str):
        candidate = (Path(directory) / library).resolve(strict=True)
        if candidate.is_file():
            return candidate
    return Path(sys.executable).resolve(strict=True)


def path_identity(path: Path) -> dict[str, object]:
    if path.is_symlink():
        raise ValueError(f"frozen source path is a symlink: {path}")
    if path.is_file():
        return file_identity(path)
    if path.is_dir():
        return tree_identity(path)
    raise ValueError(f"frozen source path is absent: {path}")


def _fsync_directory(path: Path, policy: str) -> str:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        try:
            os.fsync(descriptor)
            return "supported"
        except OSError as error:
            if error.errno not in DIRECTORY_SYNC_UNSUPPORTED or policy == "require":
                raise
            return f"unsupported-errno-{error.errno}"
    finally:
        os.close(descriptor)


def atomic_write(path: Path, payload: bytes, directory_sync_policy: str) -> str:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        os.rename(temporary, path)
        return _fsync_directory(path.parent, directory_sync_policy)
    finally:
        if temporary.exists():
            temporary.unlink()


def parse_canonical_json(payload: bytes, label: str) -> Mapping[str, object]:
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError(f"{label}: JSON is not UTF-8") from error
    value = json.loads(
        text,
        parse_constant=lambda constant: (_ for _ in ()).throw(
            ValueError(f"{label}: nonfinite JSON constant {constant}")
        ),
    )
    if not isinstance(value, dict) or canonical_json(value) + "\n" != text:
        raise ValueError(f"{label}: JSON is not a canonical object")
    return value


def read_canonical_json(path: Path) -> Mapping[str, object]:
    return parse_canonical_json(path.read_bytes(), str(path))


def require_plan_snapshot(plan_path: Path, plan_bytes: bytes) -> None:
    if plan_path.read_bytes() != plan_bytes:
        raise ValueError("campaign plan changed during the operation")


def sha256_ids(identifiers: Iterable[str]) -> tuple[int, str]:
    digest = hashlib.sha256()
    count = 0
    for identifier in identifiers:
        encoded = identifier.encode("ascii")
        digest.update(len(encoded).to_bytes(8, "big"))
        digest.update(encoded)
        count += 1
    return count, digest.hexdigest()


def scan_shard_input(
    path: Path,
    database: sqlite3.Connection | None = None,
    shard_id: str = "",
    guest_page_size: int | None = None,
    identifiers_out: list[str] | None = None,
) -> tuple[int, str]:
    def identifiers() -> Iterable[str]:
        for record in iter_raw_materializations([path]):
            obligations = representation_obligations(record)
            if obligations:
                raise ValueError(
                    f"{record['input_contract_id']}: representation obligations: "
                    + "+".join(obligations)
                )
            page_size = int(
                str(record["program"]["guest_page_size"]), 16  # type: ignore[index]
            )
            if guest_page_size is not None and page_size != guest_page_size:
                raise ValueError(
                    f"{record['input_contract_id']}: guest page size "
                    f"{page_size} differs from campaign {guest_page_size}"
                )
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
                raise ValueError(
                    f"{record['input_contract_id']}: exact engine evidence differs"
                )
            identifier = str(record["input_contract_id"])
            if INPUT_ID.fullmatch(identifier) is None:
                raise ValueError(f"invalid input contract ID {identifier!r}")
            if identifiers_out is not None:
                identifiers_out.append(identifier)
            if database is not None:
                raw_identifier = raw_contract_id(record)
                try:
                    database.execute(
                        "INSERT INTO contract_id(identifier, shard_id, raw_id) "
                        "VALUES (?, ?, ?)",
                        (identifier, shard_id, raw_identifier),
                    )
                except sqlite3.IntegrityError as error:
                    previous = database.execute(
                        "SELECT shard_id FROM contract_id WHERE identifier = ?",
                        (identifier,),
                    ).fetchone()
                    raise ValueError(
                        f"input contract {identifier} repeats in {shard_id}; "
                        f"first shard={previous[0] if previous else 'unknown'}"
                    ) from error
            yield identifier

    return sha256_ids(identifiers())


def read_tsv(path: Path, columns: Sequence[str]) -> list[dict[str, str]]:
    if path.suffix == ".gz":
        context = gzip.open(path, "rt", encoding="utf-8", newline="")
    else:
        context = path.open(encoding="utf-8", newline="")
    with context as source:
        text = source.read()
    reader = csv.DictReader(io.StringIO(text, newline=""), delimiter="\t")
    if reader.fieldnames != list(columns):
        raise ValueError(f"{path}: TSV columns differ")
    rows = list(reader)
    if not rows:
        raise ValueError(f"{path}: TSV contains no rows")
    output = io.StringIO(newline="")
    writer = csv.DictWriter(
        output,
        fieldnames=list(columns),
        delimiter="\t",
        lineterminator="\n",
    )
    writer.writeheader()
    try:
        writer.writerows(rows)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{path}: TSV rows differ") from error
    if output.getvalue() != text:
        raise ValueError(f"{path}: TSV is not canonical")
    return rows


def _digest_tuples(rows: Iterable[Sequence[str]]) -> tuple[int, str]:
    digest = hashlib.sha256()
    count = 0
    for row in rows:
        payload = canonical_json(list(row)).encode("utf-8")
        digest.update(len(payload).to_bytes(8, "big"))
        digest.update(payload)
        count += 1
    return count, digest.hexdigest()


def require_regular_file(path: Path, label: str) -> Path:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"{label} is not a nonsymlink regular file: {path}")
    return path.resolve(strict=True)


def canonical_write_target(path: Path, label: str) -> Path:
    if not path.is_absolute():
        raise ValueError(f"{label} must be absolute")
    cursor = Path(path.anchor)
    for part in path.parent.parts[1:]:
        cursor = cursor / part
        if cursor.is_symlink():
            raise ValueError(f"{label} parent contains a symlink: {cursor}")
    if not path.parent.is_dir():
        raise ValueError(f"{label} parent is not an existing directory")
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise ValueError(f"{label} is not a regular-file target")
    return path.parent.resolve(strict=True) / path.name


def validate_write_target_isolation(
    target: Path,
    *,
    forbidden_directories: Iterable[Path] = (),
    forbidden_files: Iterable[Path] = (),
    label: str,
) -> None:
    for directory in forbidden_directories:
        if _path_contains(directory, target):
            raise ValueError(f"{label} is inside protected directory {directory}")
    for path in forbidden_files:
        if paths_alias(target, path):
            raise ValueError(f"{label} aliases protected file {path}")


def resolve_manifest_owned_file(
    manifest: Path, relative: Path, label: str
) -> Path:
    if relative.is_absolute() or not relative.parts or ".." in relative.parts:
        raise ValueError(f"{label} path escapes its contract manifest")
    owner = manifest.parent.resolve(strict=True)
    cursor = owner
    for part in relative.parts:
        cursor = cursor / part
        if cursor.is_symlink():
            raise ValueError(f"{label} path contains a symlink: {cursor}")
    resolved = require_regular_file(cursor, label)
    try:
        resolved.relative_to(owner)
    except ValueError as error:
        raise ValueError(f"{label} path escapes its contract manifest") from error
    return resolved


AUTHORITATIVE_UNMAPPED_QUERY = (
    "SELECT c.identifier FROM contract_id AS c "
    "LEFT JOIN authoritative_contract_id AS a ON a.identifier = c.identifier "
    "WHERE a.identifier IS NULL LIMIT 1"
)
AUTHORITATIVE_MISSING_QUERY = (
    "SELECT a.identifier FROM authoritative_contract_id AS a "
    "LEFT JOIN contract_id AS c ON c.identifier = a.identifier "
    "WHERE c.identifier IS NULL LIMIT 1"
)


def _require_indexed_anti_join(
    database: sqlite3.Connection, query: str, searched_alias: str, label: str
) -> None:
    plan = " ".join(
        str(row)
        for row in database.execute("EXPLAIN QUERY PLAN " + query)
    )
    if (
        f"SEARCH {searched_alias} USING" not in plan
        or not any(token in plan for token in ("PRIMARY KEY", "INDEX"))
        or "AUTOMATIC" in plan
        or "USE TEMP B-TREE" in plan
    ):
        raise ValueError(f"{label} lacks its bounded indexed anti-join plan")


def require_exact_authoritative_id_set(database: sqlite3.Connection) -> None:
    _require_indexed_anti_join(
        database,
        AUTHORITATIVE_UNMAPPED_QUERY,
        "a",
        "unmapped authoritative-ID check",
    )
    _require_indexed_anti_join(
        database,
        AUTHORITATIVE_MISSING_QUERY,
        "c",
        "missing authoritative-ID check",
    )
    unmapped = database.execute(AUTHORITATIVE_UNMAPPED_QUERY).fetchone()
    missing = database.execute(AUTHORITATIVE_MISSING_QUERY).fetchone()
    if unmapped or missing:
        raise ValueError(
            "campaign shard union differs from authoritative input-ID set: "
            f"unmapped={unmapped[0] if unmapped else 'none'}, "
            f"missing={missing[0] if missing else 'none'}"
        )


def require_exact_authoritative_raw_bindings(
    database: sqlite3.Connection,
) -> None:
    mismatch = database.execute(
        "SELECT a.aggregate_id, a.input_id, a.raw_id, c.raw_id "
        "FROM authoritative_binding AS a "
        "JOIN contract_id AS c ON c.identifier = a.input_id "
        "WHERE a.raw_id != c.raw_id LIMIT 1"
    ).fetchone()
    if mismatch:
        raise ValueError(
            "campaign input/raw semantic binding differs from authority: "
            f"aggregate={mismatch[0]}, input={mismatch[1]}, "
            f"authority={mismatch[2]}, observed={mismatch[3]}"
        )


def require_unique_authoritative_raw_binding(
    database: sqlite3.Connection,
) -> None:
    conflict = database.execute(
        "SELECT input_id, MIN(raw_id), MAX(raw_id) "
        "FROM authoritative_binding GROUP BY input_id "
        "HAVING MIN(raw_id) != MAX(raw_id) LIMIT 1"
    ).fetchone()
    if conflict:
        raise ValueError(
            "canonical input has multiple authoritative raw semantic bindings: "
            f"input={conflict[0]}, first={conflict[1]}, last={conflict[2]}"
        )


def authoritative_database_identity(
    database: sqlite3.Connection,
) -> tuple[int, str, int, str]:
    contract_count, contract_digest = _digest_tuples(
        (str(row[0]),)
        for row in database.execute(
            "SELECT identifier FROM authoritative_contract_id ORDER BY identifier"
        )
    )
    binding_count, binding_digest = _digest_tuples(
        tuple(str(value) for value in row)
        for row in database.execute(
            "SELECT aggregate_id, input_id, raw_id FROM authoritative_binding "
            "ORDER BY aggregate_id, input_id, raw_id"
        )
    )
    return contract_count, contract_digest, binding_count, binding_digest


def _iter_frozen_checkpoint_contributions(
    path: Path,
) -> Iterator[Mapping[str, object]]:
    """Read a checkpoint already verified by verify_contribution_checkpoint."""
    saw_header = False
    saw_footer = False
    with gzip.open(path, "rb") as source:
        for line_number, encoded in enumerate(source, 1):
            if not encoded.endswith(b"\n"):
                raise ValueError(f"{path}:{line_number}: unterminated checkpoint row")
            try:
                record = json.loads(encoded)
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                raise ValueError(
                    f"{path}:{line_number}: checkpoint changed during merge"
                ) from error
            if (
                not isinstance(record, dict)
                or (canonical_json(record) + "\n").encode("utf-8") != encoded
            ):
                raise ValueError(
                    f"{path}:{line_number}: checkpoint changed during merge"
                )
            schema = record.get("schema")
            if line_number == 1:
                if schema != CONTRIBUTION_CHECKPOINT_HEADER_SCHEMA:
                    raise ValueError(f"{path}: checkpoint header changed during merge")
                saw_header = True
                continue
            if schema == CONTRIBUTION_CHECKPOINT_FOOTER_SCHEMA:
                if saw_footer:
                    raise ValueError(f"{path}: checkpoint footer repeats during merge")
                saw_footer = True
                continue
            if saw_footer or schema != CONTRACT_INDEX_SCHEMA:
                raise ValueError(f"{path}: checkpoint body changed during merge")
            yield record
    if not saw_header or not saw_footer:
        raise ValueError(f"{path}: checkpoint changed during merge")


def _merge_sorted_contribution_iterators(
    iterators: Sequence[Iterator[Mapping[str, object]]],
) -> Iterator[Mapping[str, object]]:
    heap: list[
        tuple[bytes, str, str, int, Mapping[str, object]]
    ] = []

    def push(index: int) -> None:
        try:
            row = next(iterators[index])
        except StopIteration:
            return
        try:
            key = (
                bytes.fromhex(str(row["sort_key"])),
                str(row["aggregate_id"]),
                str(row["raw_contract_id"]),
                index,
                row,
            )
        except (KeyError, ValueError) as error:
            raise ValueError("checkpoint merge key differs") from error
        heapq.heappush(heap, key)

    try:
        for index in range(len(iterators)):
            push(index)
        while heap:
            _sort_key, _aggregate_id, _raw_id, index, row = heapq.heappop(heap)
            yield row
            push(index)
    finally:
        for iterator in iterators:
            iterator.close()


def _iter_checkpoint_merge_run(path: Path) -> Iterator[Mapping[str, object]]:
    with gzip.open(path, "rb") as source:
        for line_number, encoded in enumerate(source, 1):
            if not encoded.endswith(b"\n"):
                raise ValueError(f"{path}:{line_number}: unterminated merge row")
            try:
                row = json.loads(encoded)
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                raise ValueError(f"{path}:{line_number}: invalid merge row") from error
            if (
                not isinstance(row, dict)
                or (canonical_json(row) + "\n").encode("utf-8") != encoded
            ):
                raise ValueError(f"{path}:{line_number}: noncanonical merge row")
            yield row


class _ScratchBudget:
    def __init__(
        self,
        maximum_bytes: int,
        before_write: Callable[[int], None] | None,
    ) -> None:
        self.maximum_bytes = _strict_integer(
            maximum_bytes, "checkpoint merge scratch cap", 1
        )
        self.before_write = before_write
        self.used_bytes = 0

    def reserve(self, byte_count: int) -> None:
        _strict_integer(byte_count, "checkpoint merge write size")
        if byte_count == 0:
            return
        if self.before_write is not None:
            self.before_write(byte_count)
        if self.used_bytes + byte_count > self.maximum_bytes:
            raise ValueError("checkpoint merge scratch exceeded its hard cap")
        self.used_bytes += byte_count

    def release(self, byte_count: int) -> None:
        _strict_integer(byte_count, "checkpoint merge release size")
        if byte_count == 0:
            return
        if byte_count > self.used_bytes:
            raise ValueError("checkpoint merge scratch accounting underflow")
        self.used_bytes -= byte_count


class _BudgetedBinaryWriter:
    def __init__(self, raw: object, budget: _ScratchBudget) -> None:
        self.raw = raw
        self.budget = budget

    def write(self, payload: bytes) -> int:
        self.budget.reserve(len(payload))
        try:
            written = self.raw.write(payload)  # type: ignore[attr-defined]
        except BaseException:
            self.budget.release(len(payload))
            raise
        if written != len(payload):
            self.budget.release(len(payload) - written)
        return written

    def flush(self) -> None:
        self.raw.flush()  # type: ignore[attr-defined]

    def tell(self) -> int:
        return int(self.raw.tell())  # type: ignore[attr-defined]


def _write_checkpoint_merge_run(
    path: Path,
    rows: Iterable[Mapping[str, object]],
    progress_callback: Callable[[int], None] | None,
    scratch_budget: _ScratchBudget | None = None,
) -> None:
    count = 0
    with path.open("xb") as raw:
        output_file: object = (
            raw if scratch_budget is None else _BudgetedBinaryWriter(raw, scratch_budget)
        )
        with gzip.GzipFile(
            filename="", mode="wb", compresslevel=1, fileobj=output_file, mtime=0
        ) as output:
            for row in rows:
                output.write((canonical_json(row) + "\n").encode("utf-8"))
                count += 1
                if progress_callback is not None and count % 65536 == 0:
                    progress_callback(count)
    if progress_callback is not None:
        progress_callback(count)


def merge_frozen_checkpoint_contributions(
    paths: Sequence[Path],
    *,
    scratch_root: Path | None = None,
    progress_callback: Callable[[int], None] | None = None,
    fan_in: int = MAX_CHECKPOINT_MERGE_FAN_IN,
    maximum_scratch_bytes: int | None = None,
    before_scratch_write: Callable[[int], None] | None = None,
) -> Iterator[Mapping[str, object]]:
    """Bounded-fan-in merge with at most ``fan_in`` live input streams."""
    if fan_in < 2 or fan_in > MAX_CHECKPOINT_MERGE_FAN_IN:
        raise ValueError("checkpoint merge fan-in is outside the safe bound")
    if not paths:
        raise ValueError("checkpoint merge has no inputs")
    with tempfile.TemporaryDirectory(
        prefix="a64-authority-merge-", dir=scratch_root
    ) as temporary:
        temporary_root = Path(temporary)
        scratch_budget = (
            None
            if maximum_scratch_bytes is None
            else _ScratchBudget(maximum_scratch_bytes, before_scratch_write)
        )
        streams: list[tuple[Path, bool]] = [(path, False) for path in paths]
        pass_index = 0
        while len(streams) > fan_in:
            merged: list[tuple[Path, bool]] = []
            for group_index, offset in enumerate(range(0, len(streams), fan_in)):
                group = streams[offset : offset + fan_in]
                iterators = [
                    (
                        _iter_checkpoint_merge_run(path)
                        if owned
                        else _iter_frozen_checkpoint_contributions(path)
                    )
                    for path, owned in group
                ]
                run = temporary_root / (
                    f"pass-{pass_index:04d}-run-{group_index:08d}.jsonl.gz"
                )
                _write_checkpoint_merge_run(
                    run,
                    _merge_sorted_contribution_iterators(iterators),
                    progress_callback,
                    scratch_budget,
                )
                merged.append((run, True))
                for path, owned in group:
                    if owned:
                        if scratch_budget is not None:
                            scratch_budget.release(path.stat().st_size)
                        path.unlink()
            streams = merged
            pass_index += 1
        final_iterators = [
            (
                _iter_checkpoint_merge_run(path)
                if owned
                else _iter_frozen_checkpoint_contributions(path)
            )
            for path, owned in streams
        ]
        yield from _merge_sorted_contribution_iterators(final_iterators)


@contextmanager
def _database_progress(
    database: sqlite3.Connection,
    callback: Callable[[int], None] | None,
):
    failure: list[Exception] = []
    calls = 0

    def progress() -> int:
        nonlocal calls
        calls += 1
        if callback is None:
            return 0
        try:
            callback(calls * 1_000_000)
        except Exception as error:
            failure.append(error)
            return 1
        return 0

    database.set_progress_handler(progress, 1_000_000)
    try:
        yield
    except sqlite3.OperationalError as error:
        if failure:
            raise failure[0] from error
        raise
    finally:
        database.set_progress_handler(None, 0)


def load_authoritative_input(
    *,
    guest_page_size: int,
    aggregate_manifest: Path,
    aggregate_manifest_identity: Path,
    materialization_index: Path,
    contract_manifests: Sequence[Path],
    database: sqlite3.Connection,
    require_campaign_set: bool = True,
    progress_callback: Callable[[int], None] | None = None,
    maximum_merge_scratch_bytes: int | None = None,
    before_merge_scratch_write: Callable[[int], None] | None = None,
) -> Mapping[str, object]:
    aggregate_manifest = require_regular_file(
        aggregate_manifest, "aggregate manifest"
    )
    aggregate_manifest_identity = require_regular_file(
        aggregate_manifest_identity, "aggregate manifest identity"
    )
    materialization_index = require_regular_file(
        materialization_index, "materialization index"
    )
    aggregate_manifest_file_identity = file_identity(aggregate_manifest)
    aggregate_identity_file_identity = file_identity(aggregate_manifest_identity)
    materialization_index_file_identity = file_identity(materialization_index)
    aggregates = read_tsv(aggregate_manifest, AGGREGATE_COLUMNS)
    frozen_identity = read_manifest_identity(aggregate_manifest_identity)
    validate_manifest_identity(aggregates, AGGREGATE_COLUMNS, frozen_identity)
    scoped = {
        row["aggregate_id"]: row
        for row in aggregates
        if int(row["guest_page_size"]) == guest_page_size
    }
    if not scoped:
        raise ValueError("aggregate manifest lacks requested guest page profile")
    index_rows = read_tsv(materialization_index, MATERIALIZATION_INDEX_COLUMNS)
    index = {row["aggregate_id"]: row for row in index_rows}
    if len(index) != len(index_rows) or set(index) != set(scoped):
        raise ValueError(
            "materialization index is not exact complete page-profile coverage"
        )
    eligible: set[str] = set()
    for aggregate_id, row in index.items():
        manifest = scoped[aggregate_id]
        if (
            row["validity_partition"] != manifest["validity_partition"]
            or row["declared_count"] != manifest["declared_count"]
            or row["declared_digest"] != manifest["declared_digest"]
            or row["observed_digest"] != manifest["observed_digest"]
            or row["decode_partition_id"] != manifest["decode_partition_id"]
            or row["mapping_status"] != "exact-allocation"
            or row["allocation_behavior_complete"] != "0"
        ):
            raise ValueError(f"{aggregate_id}: materialization index binding differs")
        if manifest["validity_partition"] == "allocated-valid":
            if (
                row["materialization_status"] != "materialized-execution-ready"
                or row["official_semantic_status"]
                != "pending-official-asl-execution"
                or row["representation_status"] != "representable"
                or row["representation_obligations"] != "none"
            ):
                raise ValueError(
                    f"{aggregate_id}: allocated input is not execution-ready"
                )
            eligible.add(aggregate_id)
    if not eligible:
        raise ValueError("page profile has no execution-ready aggregate inputs")

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
    producers: set[str] = set()
    join_model_digests: set[str] = set()
    checkpoint_rows: list[tuple[str, ...]] = []
    checkpoint_files: dict[str, Mapping[str, object]] = {}
    checkpoint_paths: list[Path] = []
    verified_contribution_count = 0
    sorted_manifests = sorted(
        require_regular_file(path, "authoritative contract manifest")
        for path in contract_manifests
    )
    if len(sorted_manifests) != len(set(sorted_manifests)):
        raise ValueError("authoritative contract manifests repeat")
    contract_manifest_identities = {
        str(path): file_identity(path) for path in sorted_manifests
    }
    for manifest_path in sorted_manifests:
        for row in read_tsv(manifest_path, CONTRACT_MANIFEST_COLUMNS):
            if row["schema"] != CONTRACT_MANIFEST_SCHEMA:
                raise ValueError(f"{manifest_path}: contract manifest schema differs")
            if int(row["guest_page_size"]) != guest_page_size:
                raise ValueError("authoritative checkpoint page profile differs")
            producer = row["producer"]
            if producer in producers:
                raise ValueError(f"authoritative producer repeats: {producer}")
            producers.add(producer)
            join_model_digests.add(row["join_model_digest"])
            relative = Path(row["checkpoint_file"])
            checkpoint = resolve_manifest_owned_file(
                manifest_path, relative, "authoritative checkpoint"
            )
            checkpoint_identity = file_identity(checkpoint)
            checkpoint_path = str(checkpoint_identity["path"])
            if checkpoint_path in checkpoint_files:
                raise ValueError("authoritative checkpoint file repeats")
            checkpoint_files[checkpoint_path] = checkpoint_identity

            info = verify_contribution_checkpoint(checkpoint)
            expected_info = {
                "producer": info.producer,
                "checkpoint_file": row["checkpoint_file"],
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
            if any(row[name] != value for name, value in expected_info.items()):
                raise ValueError(f"{manifest_path}: checkpoint manifest binding differs")
            checkpoint_rows.append(tuple(row[column] for column in CONTRACT_MANIFEST_COLUMNS))
            checkpoint_paths.append(checkpoint)
            verified_contribution_count += info.contribution_count
            if progress_callback is not None:
                progress_callback(verified_contribution_count)
    if len(join_model_digests) != 1:
        raise ValueError("authoritative checkpoints use multiple join models")
    accumulators: dict[str, AggregateAccumulator] = {}
    observed: set[str] = set()
    contribution_count = 0
    database_path = str(
        database.execute("PRAGMA database_list").fetchone()[2]
    )
    scratch_root = Path(database_path).parent if database_path else None
    for contribution in merge_frozen_checkpoint_contributions(
        checkpoint_paths,
        scratch_root=scratch_root,
        progress_callback=progress_callback,
        maximum_scratch_bytes=maximum_merge_scratch_bytes,
        before_scratch_write=before_merge_scratch_write,
    ):
        aggregate_id = str(contribution["aggregate_id"])
        if aggregate_id not in index:
            raise ValueError(
                f"checkpoint maps unknown page aggregate {aggregate_id}"
            )
        observed.add(aggregate_id)
        accumulator = accumulators.setdefault(aggregate_id, AggregateAccumulator())
        accumulator.add(contribution)
        if aggregate_id in eligible:
            obligations = contribution["representation_obligations"]
            if obligations != []:
                raise ValueError(
                    f"{aggregate_id}: authoritative input has obligations"
                )
            input_id = str(contribution["input_materialization_id"])
            raw_id = str(contribution["raw_contract_id"])
            database.execute(
                "INSERT OR IGNORE INTO authoritative_contract_id VALUES (?)",
                (input_id,),
            )
            try:
                database.execute(
                    "INSERT INTO authoritative_binding VALUES (?, ?, ?)",
                    (aggregate_id, input_id, raw_id),
                )
            except sqlite3.IntegrityError as error:
                raise ValueError(
                    "authoritative checkpoint contribution repeats"
                ) from error
        contribution_count += 1
        if progress_callback is not None and contribution_count % 65536 == 0:
            progress_callback(contribution_count)
    if progress_callback is not None:
        progress_callback(contribution_count)
    frozen_authority_files = [
        aggregate_manifest_file_identity,
        aggregate_identity_file_identity,
        materialization_index_file_identity,
        *(contract_manifest_identities[str(path)] for path in sorted_manifests),
        *(checkpoint_files[str(path.resolve())] for path in checkpoint_paths),
    ]
    for expected_file in frozen_authority_files:
        path = Path(str(expected_file["path"]))
        if file_identity(path) != expected_file:
            raise ValueError(
                f"authoritative input changed during merge: {path}"
            )
    if observed != set(index):
        raise ValueError(
            "authoritative checkpoints do not cover exact index aggregates"
        )
    for aggregate_id in sorted(index):
        accumulator = accumulators[aggregate_id]
        accumulator.finish()
        obligations = "+".join(sorted(accumulator.obligations)) or "none"
        recomputed = {
            "input_contract_count": str(accumulator.contract_count),
            "raw_contract_id_count": str(accumulator.contract_count),
            "raw_contract_id_digest": accumulator.raw_id_digest.hexdigest(),
            "input_materialization_id_count": str(accumulator.contract_count),
            "input_materialization_id_digest": (
                accumulator.input_id_digest.hexdigest()
            ),
            "representation_obligations": obligations,
            "declared_count": str(accumulator.declared_count),
            "declared_digest": accumulator.declared_digest.hexdigest(),
        }
        if any(
            index[aggregate_id][name] != value
            for name, value in recomputed.items()
        ):
            raise ValueError(
                f"{aggregate_id}: authoritative checkpoint aggregate "
                "declarations/identities differ from frozen index"
            )
    bound_aggregates = {
        str(row[0])
        for row in database.execute(
            "SELECT DISTINCT aggregate_id FROM authoritative_binding"
        )
    }
    if bound_aggregates != eligible:
        raise ValueError("execution-ready aggregates lack authoritative inputs")
    with _database_progress(database, progress_callback):
        database.execute(
            "CREATE INDEX authoritative_binding_by_input "
            "ON authoritative_binding(input_id, raw_id)"
        )
        require_unique_authoritative_raw_binding(database)
        if require_campaign_set:
            require_exact_authoritative_id_set(database)
            require_exact_authoritative_raw_bindings(database)
        (
            authoritative_count,
            authoritative_digest,
            binding_count,
            binding_digest,
        ) = authoritative_database_identity(database)
    checkpoint_count, checkpoint_digest = _digest_tuples(sorted(checkpoint_rows))
    return {
        "schema": "uml.a64.aslref-authoritative-input/v1",
        "guest_page_size": guest_page_size,
        "aggregate_manifest": aggregate_manifest_file_identity,
        "aggregate_manifest_identity": dict(frozen_identity),
        "aggregate_manifest_identity_file": aggregate_identity_file_identity,
        "materialization_index": materialization_index_file_identity,
        "contract_manifests": [
            contract_manifest_identities[str(path)] for path in sorted_manifests
        ],
        "checkpoint_files": [
            checkpoint_files[path] for path in sorted(checkpoint_files)
        ],
        "join_model_digest": next(iter(join_model_digests)),
        "page_aggregate_count": len(scoped),
        "eligible_aggregate_count": len(eligible),
        "input_contract_count": authoritative_count,
        "input_contract_ids_sha256": authoritative_digest,
        "aggregate_input_binding_count": binding_count,
        "aggregate_input_binding_sha256": binding_digest,
        "checkpoint_count": checkpoint_count,
        "checkpoint_manifest_sha256": checkpoint_digest,
    }


def validate_plan(value: Mapping[str, object]) -> Mapping[str, object]:
    expected = {
        "schema",
        "campaign_id",
        "guest_page_size",
        "shard_contract_cap",
        "total_contracts",
        "source_freeze",
        "authoritative_input",
        "probe",
        "shards",
    }
    if set(value) != expected or value.get("schema") != PLAN_SCHEMA:
        raise ValueError("campaign plan schema/fields differ")
    if not isinstance(value["campaign_id"], str) or re.fullmatch(
        r"[a-z0-9][a-z0-9.-]{0,79}", value["campaign_id"]
    ) is None:
        raise ValueError("campaign ID is invalid")
    if (
        isinstance(value["guest_page_size"], bool)
        or not isinstance(value["guest_page_size"], int)
        or value["guest_page_size"] not in {4096, 16384}
    ):
        raise ValueError("campaign guest page size differs")
    if (
        isinstance(value["shard_contract_cap"], bool)
        or not isinstance(value["shard_contract_cap"], int)
        or value["shard_contract_cap"] != MAX_SHARD_CONTRACTS
    ):
        raise ValueError("campaign shard cap differs from 1024")
    freezes = value["source_freeze"]
    if not isinstance(freezes, list) or not freezes:
        raise ValueError("campaign source freeze is empty")
    freeze_paths: list[str] = []
    for entry in freezes:
        if not isinstance(entry, dict) or set(entry) != {
            "kind", "path", "size", "file_count", "sha256"
        }:
            raise ValueError("campaign source-freeze entry differs")
        if entry["kind"] not in {"file", "tree", "python-runtime"}:
            raise ValueError("campaign source-freeze kind differs")
        if not isinstance(entry["path"], str) or not Path(entry["path"]).is_absolute():
            raise ValueError("campaign source-freeze path is not absolute")
        if str(Path(entry["path"]).resolve(strict=True)) != entry["path"]:
            raise ValueError("campaign source-freeze path is not canonical")
        _strict_integer(entry["size"], "campaign source-freeze size")
        _strict_integer(
            entry["file_count"], "campaign source-freeze file count", 1
        )
        if not isinstance(entry["sha256"], str) or SHA256.fullmatch(entry["sha256"]) is None:
            raise ValueError("campaign source-freeze digest differs")
        freeze_paths.append(entry["path"])
    if freeze_paths != sorted(set(freeze_paths)):
        raise ValueError("campaign source-freeze paths are not sorted unique")

    authoritative = value["authoritative_input"]
    authoritative_keys = {
        "schema", "guest_page_size", "aggregate_manifest",
        "aggregate_manifest_identity", "aggregate_manifest_identity_file",
        "materialization_index", "contract_manifests", "join_model_digest",
        "checkpoint_files",
        "page_aggregate_count", "eligible_aggregate_count",
        "input_contract_count", "input_contract_ids_sha256",
        "aggregate_input_binding_count", "aggregate_input_binding_sha256",
        "checkpoint_count", "checkpoint_manifest_sha256",
    }
    if (
        not isinstance(authoritative, dict)
        or set(authoritative) != authoritative_keys
        or authoritative["schema"]
        != "uml.a64.aslref-authoritative-input/v1"
    ):
        raise ValueError("campaign authoritative-input binding differs")
    _strict_integer(
        authoritative["guest_page_size"],
        "campaign authoritative guest page size",
        4096,
        16384,
    )
    if authoritative["guest_page_size"] != value["guest_page_size"]:
        raise ValueError("campaign authoritative-input page binding differs")
    for name in (
        "page_aggregate_count", "eligible_aggregate_count",
        "input_contract_count", "aggregate_input_binding_count",
        "checkpoint_count",
    ):
        _strict_integer(
            authoritative[name], f"campaign authoritative {name}", 1
        )
    for name in (
        "join_model_digest", "input_contract_ids_sha256",
        "aggregate_input_binding_sha256", "checkpoint_manifest_sha256",
    ):
        if not isinstance(authoritative[name], str) or SHA256.fullmatch(
            authoritative[name]
        ) is None:
            raise ValueError(f"campaign authoritative {name} differs")
    for name in (
        "aggregate_manifest", "aggregate_manifest_identity_file",
        "materialization_index",
    ):
        entry = authoritative[name]
        if not isinstance(entry, dict) or set(entry) != {
            "kind", "path", "size", "file_count", "sha256"
        } or entry["kind"] != "file":
            raise ValueError(f"campaign authoritative {name} identity differs")
        if (
            not isinstance(entry["path"], str)
            or not Path(entry["path"]).is_absolute()
            or not isinstance(entry["sha256"], str)
            or SHA256.fullmatch(entry["sha256"]) is None
        ):
            raise ValueError(f"campaign authoritative {name} identity differs")
        _strict_integer(entry["size"], f"campaign authoritative {name} size")
        if entry["file_count"] != 1 or isinstance(entry["file_count"], bool):
            raise ValueError(f"campaign authoritative {name} file count differs")
    if not isinstance(authoritative["aggregate_manifest_identity"], dict):
        raise ValueError("campaign aggregate-manifest identity differs")
    contract_manifests = authoritative["contract_manifests"]
    if not isinstance(contract_manifests, list) or not contract_manifests:
        raise ValueError("campaign authoritative contract manifests are empty")
    contract_paths = []
    for entry in contract_manifests:
        if not isinstance(entry, dict) or set(entry) != {
            "kind", "path", "size", "file_count", "sha256"
        } or entry["kind"] != "file":
            raise ValueError("campaign contract-manifest identity differs")
        if (
            not isinstance(entry["path"], str)
            or not Path(entry["path"]).is_absolute()
            or not isinstance(entry["sha256"], str)
            or SHA256.fullmatch(entry["sha256"]) is None
        ):
            raise ValueError("campaign contract-manifest identity differs")
        _strict_integer(entry["size"], "campaign contract-manifest size")
        if entry["file_count"] != 1 or isinstance(entry["file_count"], bool):
            raise ValueError("campaign contract-manifest file count differs")
        contract_paths.append(entry["path"])
    if contract_paths != sorted(set(contract_paths)):
        raise ValueError("campaign contract-manifest paths are not sorted unique")
    checkpoint_files = authoritative["checkpoint_files"]
    if not isinstance(checkpoint_files, list) or not checkpoint_files:
        raise ValueError("campaign authoritative checkpoint files are empty")
    checkpoint_paths = []
    for entry in checkpoint_files:
        if not isinstance(entry, dict) or set(entry) != {
            "kind", "path", "size", "file_count", "sha256"
        } or entry["kind"] != "file":
            raise ValueError("campaign checkpoint-file identity differs")
        if (
            not isinstance(entry["path"], str)
            or not Path(entry["path"]).is_absolute()
            or not isinstance(entry["sha256"], str)
            or SHA256.fullmatch(entry["sha256"]) is None
        ):
            raise ValueError("campaign checkpoint-file identity differs")
        _strict_integer(entry["size"], "campaign checkpoint-file size")
        if entry["file_count"] != 1 or isinstance(entry["file_count"], bool):
            raise ValueError("campaign checkpoint-file count differs")
        checkpoint_paths.append(entry["path"])
    if checkpoint_paths != sorted(set(checkpoint_paths)):
        raise ValueError("campaign checkpoint-file paths are not sorted unique")

    probe = value["probe"]
    probe_keys = {
        "path", "python", "python_library", "python_runtime", "architecture_root",
        "aslref", "bundler", "features_json",
        "closure_capture_directory", "required_variants", "interface",
        "observer", "timeout_seconds", "max_rss_kib", "max_encoded_bytes",
        "max_artifact_bytes", "max_artifact_files", "max_log_bytes",
    }
    if not isinstance(probe, dict) or set(probe) != probe_keys:
        raise ValueError("campaign probe configuration differs")
    numeric_probe_keys = {
        "timeout_seconds", "max_rss_kib", "max_encoded_bytes",
        "max_artifact_bytes", "max_artifact_files", "max_log_bytes",
    }
    for name in probe_keys - numeric_probe_keys:
        if not isinstance(probe[name], str) or not Path(probe[name]).is_absolute():
            raise ValueError(f"campaign probe {name} is not an absolute path")
    for name in numeric_probe_keys:
        _strict_integer(probe[name], f"campaign probe {name}", 1)

    freeze_by_path = {
        str(entry["path"]): entry for entry in freezes
    }
    required_probe_kinds = {
        "path": "file",
        "python": "file",
        "python_library": "file",
        "python_runtime": "python-runtime",
        "architecture_root": "tree",
        "aslref": "file",
        "bundler": "file",
        "features_json": "file",
        "closure_capture_directory": "tree",
        "required_variants": "file",
        "interface": "file",
        "observer": "file",
    }
    for name, kind in required_probe_kinds.items():
        path = str(probe[name])
        if path not in freeze_by_path or freeze_by_path[path]["kind"] != kind:
            raise ValueError(
                f"campaign probe {name} lacks an exact {kind} source freeze"
            )
    interface_parent = str(Path(str(probe["interface"])).parent)
    if (
        interface_parent not in freeze_by_path
        or freeze_by_path[interface_parent]["kind"] != "tree"
    ):
        raise ValueError("campaign probe module directory lacks a source freeze")
    for name in (
        "aggregate_manifest",
        "aggregate_manifest_identity_file",
        "materialization_index",
    ):
        entry = authoritative[name]
        assert isinstance(entry, dict)
        if freeze_by_path.get(str(entry["path"])) != entry:
            raise ValueError(
                f"campaign authoritative {name} lacks its exact source freeze"
            )
    for entry in contract_manifests:
        assert isinstance(entry, dict)
        if freeze_by_path.get(str(entry["path"])) != entry:
            raise ValueError(
                "campaign authoritative contract manifest lacks its exact "
                "source freeze"
            )
    for entry in checkpoint_files:
        assert isinstance(entry, dict)
        if freeze_by_path.get(str(entry["path"])) != entry:
            raise ValueError(
                "campaign authoritative checkpoint file lacks its exact "
                "source freeze"
            )

    shards = value["shards"]
    if not isinstance(shards, list) or not shards:
        raise ValueError("campaign has no shards")
    total = 0
    paths: list[str] = []
    ids: list[str] = []
    for index, shard in enumerate(shards):
        if not isinstance(shard, dict) or set(shard) != {
            "shard_id", "input", "contract_count", "input_contract_ids_sha256"
        }:
            raise ValueError("campaign shard fields differ")
        shard_id = shard["shard_id"]
        if not isinstance(shard_id, str) or SHARD_ID.fullmatch(shard_id) is None:
            raise ValueError("campaign shard ID differs")
        if shard_id != f"shard-{index:08d}":
            raise ValueError("campaign shard IDs are not contiguous canonical IDs")
        source = shard["input"]
        if not isinstance(source, dict) or set(source) != {
            "path", "size", "sha256"
        }:
            raise ValueError(f"{shard_id}: input identity differs")
        if not isinstance(source["path"], str) or not Path(source["path"]).is_absolute():
            raise ValueError(f"{shard_id}: input path is not absolute")
        if str(Path(source["path"]).resolve(strict=True)) != source["path"]:
            raise ValueError(f"{shard_id}: input path is not canonical")
        _strict_integer(source["size"], f"{shard_id}: input size", 1)
        if not isinstance(source["sha256"], str) or SHA256.fullmatch(source["sha256"]) is None:
            raise ValueError(f"{shard_id}: input digest differs")
        count = shard["contract_count"]
        _strict_integer(
            count,
            f"{shard_id}: contract count",
            1,
            MAX_SHARD_CONTRACTS,
        )
        if not isinstance(shard["input_contract_ids_sha256"], str) or SHA256.fullmatch(
            shard["input_contract_ids_sha256"]
        ) is None:
            raise ValueError(f"{shard_id}: contract-ID digest differs")
        total += count
        ids.append(shard_id)
        paths.append(source["path"])
    if ids != sorted(set(ids)) or paths != sorted(set(paths)):
        raise ValueError("campaign shard IDs/input paths are not sorted unique")
    _strict_integer(value["total_contracts"], "campaign total contracts", 1)
    if value["total_contracts"] != total:
        raise ValueError("campaign total contract count differs")
    if value["total_contracts"] != authoritative["input_contract_count"]:
        raise ValueError("campaign total differs from authoritative input set")
    return value


def verify_source_freeze(plan: Mapping[str, object]) -> None:
    for expected in plan["source_freeze"]:  # type: ignore[index]
        assert isinstance(expected, dict)
        if expected["kind"] == "python-runtime":
            observed = python_runtime_identity(Path(str(expected["path"])))
        else:
            observed = path_identity(Path(str(expected["path"])))
        if observed != expected:
            raise ValueError(f"frozen source changed: {expected['path']}")


def require_authority_capacity(
    local_temp_root: Path | None,
    work_root: Path | None,
    minimum_local_free_gib: float,
    maximum_database_bytes: int,
    maximum_merge_scratch_bytes: int,
) -> None:
    maximum_database_bytes = _strict_integer(
        maximum_database_bytes, "authority database cap", 4096
    )
    maximum_merge_scratch_bytes = _strict_integer(
        maximum_merge_scratch_bytes, "authority merge scratch cap", 1
    )
    if local_temp_root is None:
        return
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
        raise ValueError("authority local work exceeded its hard cap")
    require_free_space(
        local_temp_root,
        minimum_local_free_gib,
        "authority verification local",
        headroom_bytes=maximum - used,
    )


def verify_authoritative_plan(
    plan: Mapping[str, object],
    *,
    local_temp_root: Path | None = None,
    minimum_local_free_gib: float = 0.0,
    maximum_database_bytes: int = DEFAULT_MAX_AUTHORITY_DATABASE_BYTES,
    maximum_merge_scratch_bytes: int = (
        DEFAULT_MAX_AUTHORITY_MERGE_SCRATCH_BYTES
    ),
) -> None:
    authoritative = plan["authoritative_input"]
    assert isinstance(authoritative, dict)
    if local_temp_root is not None:
        local_temp_root = require_root(
            local_temp_root, "authority verification local temp root"
        )
        require_authority_capacity(
            local_temp_root,
            None,
            minimum_local_free_gib,
            maximum_database_bytes,
            maximum_merge_scratch_bytes,
        )
    with tempfile.TemporaryDirectory(
        prefix="a64-campaign-authority-", dir=local_temp_root
    ) as temporary:
        temporary_root = Path(temporary)
        database_path = temporary_root / "authority.sqlite3"
        database = sqlite3.connect(database_path)
        database.execute("PRAGMA journal_mode=OFF")
        database.execute("PRAGMA synchronous=OFF")
        configure_bounded_sqlite_storage(
            database, database_path, maximum_database_bytes
        )
        database.execute(
            "CREATE TABLE contract_id(identifier TEXT PRIMARY KEY, "
            "shard_id TEXT NOT NULL, raw_id TEXT NOT NULL)"
        )
        shards = plan["shards"]
        assert isinstance(shards, list)
        for shard in shards:
            source = shard["input"]
            assert isinstance(source, dict)
            count, digest = scan_shard_input(
                Path(str(source["path"])),
                database,
                str(shard["shard_id"]),
                int(plan["guest_page_size"]),
            )
            if (
                count != shard["contract_count"]
                or digest != shard["input_contract_ids_sha256"]
            ):
                raise ValueError(
                    f"{shard['shard_id']}: frozen campaign shard identity differs"
                )
            require_authority_capacity(
                local_temp_root,
                temporary_root,
                minimum_local_free_gib,
                maximum_database_bytes,
                maximum_merge_scratch_bytes,
            )
        observed = load_authoritative_input(
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
                else lambda _count: require_authority_capacity(
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
                else lambda _bytes: require_authority_capacity(
                    local_temp_root,
                    temporary_root,
                    minimum_local_free_gib,
                    maximum_database_bytes,
                    maximum_merge_scratch_bytes,
                )
            ),
        )
        database.close()
    if observed != authoritative:
        changed = sorted(
            name
            for name in observed
            if observed[name] != authoritative.get(name)
        )
        raise ValueError(
            "campaign authoritative input binding changed: " + ",".join(changed)
        )


def plan_arguments(parser: argparse.ArgumentParser) -> None:
    repository = Path(__file__).resolve().parents[3]
    parser.add_argument("--campaign-id", required=True)
    parser.add_argument("--input-list", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--local-temp-root", type=Path, required=True)
    parser.add_argument("--minimum-local-free-gib", type=float, default=20.0)
    parser.add_argument(
        "--maximum-authority-database-bytes",
        type=int,
        default=DEFAULT_MAX_AUTHORITY_DATABASE_BYTES,
    )
    parser.add_argument(
        "--maximum-authority-merge-scratch-bytes",
        type=int,
        default=DEFAULT_MAX_AUTHORITY_MERGE_SCRATCH_BYTES,
    )
    parser.add_argument(
        "--guest-page-size", type=int, choices=(4096, 16384), required=True
    )
    parser.add_argument("--expected-total-contracts", type=int)
    parser.add_argument(
        "--probe",
        type=Path,
        default=Path(__file__).with_name("probe_a64_aslref_materialization_batch.py"),
    )
    parser.add_argument("--architecture-root", type=Path, required=True)
    parser.add_argument("--aslref", type=Path, required=True)
    parser.add_argument("--bundler", type=Path, required=True)
    parser.add_argument("--features-json", type=Path, required=True)
    parser.add_argument("--closure-capture-directory", type=Path, required=True)
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
    parser.add_argument("--materialization-index", type=Path, required=True)
    parser.add_argument(
        "--contract-manifest", action="append", type=Path, required=True
    )
    parser.add_argument(
        "--required-variants",
        type=Path,
        default=repository / "tests/unit/arm64/a64_instruction_required_variants.tsv",
    )
    parser.add_argument(
        "--interface",
        type=Path,
        default=Path(__file__).with_name("a64_aslref_user_state_interface.asl"),
    )
    parser.add_argument(
        "--observer",
        type=Path,
        default=Path(__file__).with_name("a64_aslref_execution_observer.asl"),
    )
    parser.add_argument("--timeout-seconds", type=int, default=1800)
    parser.add_argument("--max-rss-kib", type=int, default=DEFAULT_MAX_RSS_KIB)
    parser.add_argument(
        "--max-encoded-bytes", type=int, default=DEFAULT_MAX_ENCODED_BYTES
    )
    parser.add_argument(
        "--max-artifact-bytes", type=int, default=DEFAULT_MAX_ARTIFACT_BYTES
    )
    parser.add_argument(
        "--max-artifact-files", type=int, default=DEFAULT_MAX_ARTIFACT_FILES
    )
    parser.add_argument(
        "--max-log-bytes", type=int, default=DEFAULT_MAX_LOG_BYTES
    )
    parser.add_argument("--freeze-path", action="append", type=Path, default=[])


def build_plan(options: argparse.Namespace) -> Mapping[str, object]:
    local_temp_root = require_root(options.local_temp_root, "plan local temp root")
    if (
        isinstance(options.minimum_local_free_gib, bool)
        or not isinstance(options.minimum_local_free_gib, (int, float))
        or not math.isfinite(options.minimum_local_free_gib)
        or options.minimum_local_free_gib < 0
    ):
        raise ValueError("plan local free-space reserve is invalid")
    require_free_space(
        local_temp_root, options.minimum_local_free_gib, "plan local"
    )
    require_authority_capacity(
        local_temp_root,
        None,
        options.minimum_local_free_gib,
        options.maximum_authority_database_bytes,
        options.maximum_authority_merge_scratch_bytes,
    )
    for name in (
        "timeout_seconds",
        "max_rss_kib",
        "max_encoded_bytes",
        "max_artifact_bytes",
        "max_artifact_files",
        "max_log_bytes",
    ):
        _strict_integer(
            getattr(options, name), f"campaign plan {name}", 1
        )
    output_target = canonical_write_target(
        options.output, "campaign plan output"
    )
    input_list = require_regular_file(options.input_list, "campaign input list")
    input_paths = [
        Path(line.strip()).expanduser().resolve()
        for line in input_list.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if not input_paths or input_paths != sorted(set(input_paths)):
        raise ValueError("input list must contain sorted unique shard paths")
    configured_paths = {
        "path": options.probe.resolve(),
        "python": Path(sys.executable).resolve(strict=True),
        "python_library": python_library_path(),
        "python_runtime": Path(
            sysconfig.get_path("stdlib")
        ).resolve(strict=True),
        "architecture_root": options.architecture_root.resolve(),
        "aslref": options.aslref.resolve(),
        "bundler": options.bundler.resolve(),
        "features_json": options.features_json.resolve(),
        "closure_capture_directory": options.closure_capture_directory.resolve(),
        "required_variants": options.required_variants.resolve(),
        "interface": options.interface.resolve(),
        "observer": options.observer.resolve(),
    }
    freeze_paths = {
        *configured_paths.values(),
        configured_paths["interface"].parent,
        options.aggregate_manifest.resolve(),
        options.aggregate_manifest_identity.resolve(),
        options.materialization_index.resolve(),
        *(path.resolve() for path in options.contract_manifest),
        *(path.resolve() for path in options.freeze_path),
    }
    protected_plan_inputs = [input_list, *input_paths, *freeze_paths]
    if _path_contains(local_temp_root, output_target) or _path_contains(
        output_target, local_temp_root
    ):
        raise ValueError("campaign plan output/local root overlap")
    if any(
        _path_contains(local_temp_root, path)
        or _path_contains(path, local_temp_root)
        for path in protected_plan_inputs
    ):
        raise ValueError("plan local root aliases a frozen source/input")
    preflight_directories = [
        configured_paths["python_runtime"],
        configured_paths["architecture_root"],
        configured_paths["closure_capture_directory"],
        configured_paths["interface"].parent,
    ]
    validate_write_target_isolation(
        output_target,
        forbidden_directories=preflight_directories,
        forbidden_files=[input_list, *input_paths, *freeze_paths],
        label="campaign plan output",
    )
    shards: list[dict[str, object]] = []
    total = 0
    with tempfile.TemporaryDirectory(
        prefix="a64-campaign-plan-", dir=local_temp_root
    ) as temporary:
        temporary_root = Path(temporary)
        database_path = temporary_root / "contract-ids.sqlite3"
        database = sqlite3.connect(database_path)
        database.execute("PRAGMA journal_mode=OFF")
        database.execute("PRAGMA synchronous=OFF")
        configure_bounded_sqlite_storage(
            database,
            database_path,
            options.maximum_authority_database_bytes,
        )
        database.execute(
            "CREATE TABLE contract_id(identifier TEXT PRIMARY KEY, "
            "shard_id TEXT NOT NULL, raw_id TEXT NOT NULL)"
        )
        for index, path in enumerate(input_paths):
            shard_id = f"shard-{index:08d}"
            identity = file_identity(path)
            count, digest = scan_shard_input(
                path, database, shard_id, options.guest_page_size
            )
            if count > MAX_SHARD_CONTRACTS:
                raise ValueError(f"{path}: {count} contracts exceed shard cap")
            total += count
            shards.append(
                {
                    "shard_id": shard_id,
                    "input": {
                        "path": identity["path"],
                        "size": identity["size"],
                        "sha256": identity["sha256"],
                    },
                    "contract_count": count,
                    "input_contract_ids_sha256": digest,
                }
            )
            require_authority_capacity(
                local_temp_root,
                temporary_root,
                options.minimum_local_free_gib,
                options.maximum_authority_database_bytes,
                options.maximum_authority_merge_scratch_bytes,
            )
        authoritative = load_authoritative_input(
            guest_page_size=options.guest_page_size,
            aggregate_manifest=options.aggregate_manifest.resolve(),
            aggregate_manifest_identity=(
                options.aggregate_manifest_identity.resolve()
            ),
            materialization_index=options.materialization_index.resolve(),
            contract_manifests=[
                path.resolve() for path in options.contract_manifest
            ],
            database=database,
            progress_callback=lambda _count: require_authority_capacity(
                local_temp_root,
                temporary_root,
                options.minimum_local_free_gib,
                options.maximum_authority_database_bytes,
                options.maximum_authority_merge_scratch_bytes,
            ),
            maximum_merge_scratch_bytes=(
                options.maximum_authority_merge_scratch_bytes
            ),
            before_merge_scratch_write=lambda _bytes: require_authority_capacity(
                local_temp_root,
                temporary_root,
                options.minimum_local_free_gib,
                options.maximum_authority_database_bytes,
                options.maximum_authority_merge_scratch_bytes,
            ),
        )
        database.close()
    checkpoint_files = authoritative["checkpoint_files"]
    assert isinstance(checkpoint_files, list)
    freeze_paths.update(
        Path(str(entry["path"]))
        for entry in checkpoint_files
        if isinstance(entry, Mapping)
    )
    source_freeze = [
        (
            python_runtime_identity(path)
            if path == configured_paths["python_runtime"]
            else path_identity(path)
        )
        for path in sorted(freeze_paths, key=lambda item: str(item))
    ]
    if options.expected_total_contracts is not None and total != options.expected_total_contracts:
        raise ValueError(
            f"planned {total} contracts, expected {options.expected_total_contracts}"
        )
    plan = {
        "schema": PLAN_SCHEMA,
        "campaign_id": options.campaign_id,
        "guest_page_size": options.guest_page_size,
        "shard_contract_cap": MAX_SHARD_CONTRACTS,
        "total_contracts": total,
        "source_freeze": source_freeze,
        "authoritative_input": authoritative,
        "probe": {
            **{name: str(path) for name, path in configured_paths.items()},
            "timeout_seconds": options.timeout_seconds,
            "max_rss_kib": options.max_rss_kib,
            "max_encoded_bytes": options.max_encoded_bytes,
            "max_artifact_bytes": options.max_artifact_bytes,
            "max_artifact_files": options.max_artifact_files,
            "max_log_bytes": options.max_log_bytes,
        },
        "shards": shards,
    }
    validate_plan(plan)
    protected_directories = [
        Path(str(entry["path"]))
        for entry in source_freeze
        if entry["kind"] in {"tree", "python-runtime"}
    ]
    protected_files = [
        input_list,
        *(
            Path(str(entry["path"]))
            for entry in source_freeze
            if entry["kind"] == "file"
        ),
        *(Path(str(shard["input"]["path"])) for shard in shards),
    ]
    validate_write_target_isolation(
        output_target,
        forbidden_directories=protected_directories,
        forbidden_files=protected_files,
        label="campaign plan output",
    )
    payload = (canonical_json(plan) + "\n").encode("utf-8")
    if output_target.exists():
        if output_target.read_bytes() != payload:
            raise ValueError(
                "campaign plan output already contains a different plan"
            )
        return plan
    atomic_write(
        output_target,
        payload,
        "require",
    )
    return plan


def _strict_integer(
    value: object, label: str, minimum: int = 0, maximum: int = MAX_ARTIFACT_SCALAR
) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or value < minimum
        or value > maximum
    ):
        raise ValueError(f"{label} is not a bounded integer")
    return value


def _strict_positive_number(value: object, label: str) -> float:
    if (
        isinstance(value, bool)
        or not isinstance(value, (int, float))
        or not math.isfinite(value)
        or value <= 0
        or value > float(MAX_ARTIFACT_SCALAR)
    ):
        raise ValueError(f"{label} is not a finite positive number")
    return float(value)


def _validate_sync_observation(value: object, label: str) -> str:
    if not isinstance(value, str) or (
        value != "supported"
        and re.fullmatch(r"unsupported-errno-[1-9][0-9]*", value) is None
    ):
        raise ValueError(f"{label} is not a directory-sync observation")
    return value


def validate_published_probe_semantics(
    directory: Path,
    shard: Mapping[str, object],
    guest_page_size: int,
) -> Mapping[str, str]:
    required = {
        "batch-report.json",
        "aslref-result.json",
        "aslref.stdout",
        "aslref.stderr",
        "campaign-probe.stdout",
        "campaign-probe.stderr",
    }
    for name in required:
        require_regular_file(directory / name, f"published probe {name}")

    source = shard["input"]
    if not isinstance(source, Mapping):
        raise ValueError("published shard source binding differs")
    source_path = require_regular_file(
        Path(str(source["path"])), "published shard input"
    )
    source_identity = file_identity(source_path)
    if (
        source_identity["size"] != source["size"]
        or source_identity["sha256"] != source["sha256"]
    ):
        raise ValueError("published shard input file identity changed")
    identifiers: list[str] = []
    count, identifier_digest = scan_shard_input(
        source_path,
        guest_page_size=guest_page_size,
        identifiers_out=identifiers,
    )
    if (
        count != shard["contract_count"]
        or identifier_digest != shard["input_contract_ids_sha256"]
    ):
        raise ValueError("published shard input identity differs")

    report_path = directory / "batch-report.json"
    report = read_canonical_json(report_path)
    fixed_report = {
        "schema": PROBE_FORMAT,
        "contracts": count,
        "selection": "explicit-unique-campaign-shard",
        "promotable": False,
        "diagnostic": True,
        "diagnostic_maximum_phase": "full",
        "official_closure_seed_contracts": 4,
        "executed_source_contracts": count,
        "unique_input_contracts": count,
        "scale_workflow": None,
        "maximum_backing_contract": None,
        "full_extent_transfer_contract": None,
        "full_extent_execution_policy": None,
    }
    if any(report.get(name) != value for name, value in fixed_report.items()):
        raise ValueError("published probe report success/selection differs")
    workflow = report.get("campaign_workflow")
    workflow_keys = {
        "purpose",
        "semantic_coverage_promotion",
        "unique_input_contracts",
        "failures",
        "guest_page_size",
        "input_contract_ids_sha256",
        "input_result_count",
        "input_results_sha256",
        "input_results",
        "input_sources",
    }
    if not isinstance(workflow, dict) or set(workflow) != workflow_keys:
        raise ValueError("published probe campaign workflow differs")
    expected_results = [
        {
            "input_contract_id": identifier,
            "official_asl_result": "official-asl-passed",
        }
        for identifier in identifiers
    ]
    result_count, result_digest = _digest_tuples(
        (identifier, "official-asl-passed") for identifier in identifiers
    )
    expected_workflow = {
        "purpose": "official-execution-shard-pending-campaign-join",
        "semantic_coverage_promotion": "requires-exact-frozen-join",
        "unique_input_contracts": count,
        "failures": 0,
        "guest_page_size": guest_page_size,
        "input_contract_ids_sha256": identifier_digest,
        "input_result_count": result_count,
        "input_results_sha256": result_digest,
        "input_results": expected_results,
        "input_sources": [
            {
                "path": str(source_path),
                "size": source["size"],
                "sha256": source["sha256"],
            }
        ],
    }
    if workflow != expected_workflow:
        raise ValueError("published per-input official ASL results differ")

    result_path = directory / "aslref-result.json"
    result_text = result_path.read_text(encoding="utf-8")
    result = json.loads(
        result_text,
        parse_constant=lambda constant: (_ for _ in ()).throw(
            ValueError(
                f"{result_path}: nonfinite JSON constant {constant}"
            )
        ),
    )
    result_keys = {
        "schema",
        "returncode",
        "stdout",
        "stderr",
        "elapsed_seconds",
        "peak_rss_kib",
        "rss_measurement",
        "termination_reason",
        "accepted",
        "passed",
        "promotable",
        "diagnostic",
        "diagnostic_maximum_phase",
    }
    if (
        not isinstance(result, dict)
        or set(result) != result_keys
        or json.dumps(result, indent=2, sort_keys=True) + "\n" != result_text
    ):
        raise ValueError("published ASLRef result schema/encoding differs")
    expected_stdout = (
        f"FORMAT={PROBE_FORMAT}|COUNT={count}|FAILURES=0\n"
    )
    fixed_result = {
        "schema": "uml.a64.aslref-materialization-batch-result/v1",
        "returncode": 0,
        "stdout": expected_stdout,
        "stderr": "",
        "termination_reason": "exit",
        "accepted": False,
        "passed": True,
        "promotable": False,
        "diagnostic": True,
        "diagnostic_maximum_phase": "full",
    }
    if any(result.get(name) != value for name, value in fixed_result.items()):
        raise ValueError("published ASLRef result success contract differs")
    _strict_positive_number(result["elapsed_seconds"], "ASLRef result elapsed time")
    _strict_integer(result["peak_rss_kib"], "ASLRef result peak RSS")
    if result["rss_measurement"] not in {
        "live-ps+rusage",
        "rusage-children-fallback",
    }:
        raise ValueError("published ASLRef RSS measurement differs")
    if (directory / "aslref.stdout").read_text(encoding="utf-8") != expected_stdout:
        raise ValueError("published ASLRef stdout file differs")
    if (directory / "aslref.stderr").read_bytes() != b"":
        raise ValueError("published ASLRef stderr file is not empty")
    if (
        (directory / "campaign-probe.stdout").read_text(encoding="utf-8")
        != canonical_json(report) + "\n"
        or (directory / "campaign-probe.stderr").read_bytes() != b""
    ):
        raise ValueError("published campaign probe process output differs")
    if (
        report.get("peak_rss_kib") != result["peak_rss_kib"]
        or report.get("elapsed_seconds") != result["elapsed_seconds"]
        or report.get("rss_measurement") != result["rss_measurement"]
        or report.get("termination_reason") != "exit"
    ):
        raise ValueError("published probe report/result resource binding differs")
    _strict_positive_number(report.get("contracts_per_second"), "probe throughput")
    return {
        "probe_report_sha256": str(file_identity(report_path)["sha256"]),
        "probe_result_sha256": str(file_identity(result_path)["sha256"]),
        "input_results_sha256": result_digest,
    }


def _artifact_manifest(directory: Path) -> Mapping[str, object]:
    files = []
    for path in sorted(directory.rglob("*"), key=lambda item: item.as_posix()):
        if path.is_symlink():
            raise ValueError(f"artifact contains symlink {path}")
        if not path.is_file() or path.name in {
            "artifact-manifest.json", "SUCCESS.json"
        }:
            continue
        identity = file_identity(path)
        files.append(
            {
                "path": path.relative_to(directory).as_posix(),
                "size": identity["size"],
                "sha256": identity["sha256"],
            }
        )
    if not files:
        raise ValueError("shard artifact is empty")
    return {"schema": ARTIFACT_SCHEMA, "files": files}


def _verify_artifact(directory: Path, manifest: Mapping[str, object]) -> None:
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError("artifact root is not a nonsymlink directory")
    if set(manifest) != {"schema", "files"} or manifest["schema"] != ARTIFACT_SCHEMA:
        raise ValueError("artifact manifest schema differs")
    files = manifest["files"]
    if not isinstance(files, list) or not files:
        raise ValueError("artifact manifest has no files")
    expected_paths = []
    for entry in files:
        if not isinstance(entry, dict) or set(entry) != {"path", "size", "sha256"}:
            raise ValueError("artifact manifest entry differs")
        relative = Path(str(entry["path"]))
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("artifact manifest path escapes its shard")
        path = directory / relative
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"artifact is not a nonsymlink file: {relative}")
        identity = file_identity(path)
        _strict_integer(entry["size"], f"artifact {relative} size")
        if (
            not isinstance(entry["sha256"], str)
            or SHA256.fullmatch(entry["sha256"]) is None
            or identity["size"] != entry["size"]
            or identity["sha256"] != entry["sha256"]
        ):
            raise ValueError(f"published artifact differs: {relative}")
        expected_paths.append(relative.as_posix())
    if expected_paths != sorted(set(expected_paths)):
        raise ValueError("artifact manifest paths are not sorted unique")
    actual_paths = []
    actual_directories = []
    for path in directory.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"artifact contains symlink {path}")
        if path.is_file():
            actual_paths.append(path.relative_to(directory).as_posix())
        elif path.is_dir():
            actual_directories.append(path.relative_to(directory).as_posix())
        else:
            raise ValueError(f"artifact contains unsupported entry {path}")
    metadata = {"artifact-manifest.json"}
    if (directory / "SUCCESS.json").exists():
        metadata.add("SUCCESS.json")
    if set(actual_paths) != set(expected_paths) | metadata:
        raise ValueError("artifact actual file set differs from its manifest")
    expected_directories = {
        parent.as_posix()
        for relative in map(Path, expected_paths)
        for parent in relative.parents
        if parent != Path(".")
    }
    if set(actual_directories) != expected_directories:
        raise ValueError("artifact actual directory set differs from its manifest")


def _copy_artifact(source: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=False)
    for path in sorted(source.rglob("*"), key=lambda item: item.as_posix()):
        relative = path.relative_to(source)
        target = destination / relative
        if path.is_dir():
            target.mkdir(exist_ok=False)
            continue
        if not path.is_file() or path.is_symlink():
            raise ValueError(f"unsupported shard artifact entry {path}")
        target.parent.mkdir(parents=True, exist_ok=True)
        with path.open("rb") as input_file, target.open("xb") as output_file:
            shutil.copyfileobj(input_file, output_file, 1024 * 1024)
            output_file.flush()
            os.fsync(output_file.fileno())


def _fsync_directory_tree(path: Path, policy: str) -> str:
    directories = [path]
    directories.extend(
        candidate
        for candidate in path.rglob("*")
        if candidate.is_dir() and not candidate.is_symlink()
    )
    observations = {
        _fsync_directory(directory, policy)
        for directory in sorted(
            directories,
            key=lambda item: (-len(item.parts), item.as_posix()),
        )
    }
    if len(observations) != 1:
        raise ValueError(
            "artifact directories produced inconsistent sync outcomes"
        )
    return next(iter(observations))


def verify_published_shard(
    directory: Path,
    plan_sha256: str,
    shard: Mapping[str, object],
    guest_page_size: int,
) -> Mapping[str, object]:
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError(f"{directory}: published shard is not a directory")
    for name in ("SUCCESS.json", "artifact-manifest.json"):
        path = directory / name
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"{directory}: {name} is not a nonsymlink file")
    success = read_canonical_json(directory / "SUCCESS.json")
    if set(success) != {
        "schema", "campaign_plan_sha256", "shard_id", "contract_count",
        "input_contract_ids_sha256", "artifact_manifest_sha256",
        "artifact_file_count", "artifact_bytes", "elapsed_seconds",
        "peak_rss_kib", "directory_sync_policy", "directory_sync_outcomes",
        "probe_report_sha256", "probe_result_sha256", "input_results_sha256",
    } or success["schema"] != SUCCESS_SCHEMA:
        raise ValueError(f"{directory}: shard success schema differs")
    for name in (
        "campaign_plan_sha256", "input_contract_ids_sha256",
        "artifact_manifest_sha256", "probe_report_sha256",
        "probe_result_sha256", "input_results_sha256",
    ):
        if not isinstance(success[name], str) or SHA256.fullmatch(success[name]) is None:
            raise ValueError(f"{directory}: shard success {name} differs")
    if (
        not isinstance(success["shard_id"], str)
        or SHARD_ID.fullmatch(success["shard_id"]) is None
    ):
        raise ValueError(f"{directory}: shard success ID differs")
    _strict_integer(
        success["contract_count"],
        "shard success contract count",
        1,
        MAX_SHARD_CONTRACTS,
    )
    _strict_integer(success["artifact_file_count"], "shard artifact file count", 1)
    _strict_integer(success["artifact_bytes"], "shard artifact bytes")
    _strict_positive_number(success["elapsed_seconds"], "shard elapsed time")
    _strict_integer(success["peak_rss_kib"], "shard peak RSS")
    if success["directory_sync_policy"] not in DIRECTORY_SYNC_POLICIES:
        raise ValueError(f"{directory}: shard directory-sync policy differs")
    outcomes = success["directory_sync_outcomes"]
    if not isinstance(outcomes, dict) or set(outcomes) != {
        "artifact_stage", "campaign_root", "shards_parent"
    }:
        raise ValueError(f"{directory}: shard directory-sync outcomes differ")
    for name, observation in outcomes.items():
        _validate_sync_observation(
            observation, f"shard directory-sync outcome {name}"
        )
        if (
            success["directory_sync_policy"] == "require"
            and observation != "supported"
        ):
            raise ValueError(f"{directory}: required directory sync was unsupported")
    expected = {
        "campaign_plan_sha256": plan_sha256,
        "shard_id": shard["shard_id"],
        "contract_count": shard["contract_count"],
        "input_contract_ids_sha256": shard["input_contract_ids_sha256"],
    }
    if any(success[name] != value for name, value in expected.items()):
        raise ValueError(f"{directory}: shard success binding differs")
    manifest_path = directory / "artifact-manifest.json"
    manifest_identity = file_identity(manifest_path)
    if manifest_identity["sha256"] != success["artifact_manifest_sha256"]:
        raise ValueError(f"{directory}: artifact manifest digest differs")
    manifest = read_canonical_json(manifest_path)
    _verify_artifact(directory, manifest)
    semantic_identity = validate_published_probe_semantics(
        directory, shard, guest_page_size
    )
    if any(success[name] != value for name, value in semantic_identity.items()):
        raise ValueError(f"{directory}: published probe semantic digest differs")
    files = manifest["files"]
    assert isinstance(files, list)
    if success["artifact_file_count"] != len(files):
        raise ValueError(f"{directory}: artifact file count differs")
    if success["artifact_bytes"] != sum(int(item["size"]) for item in files):
        raise ValueError(f"{directory}: artifact byte count differs")
    return success


def require_resume_durability(
    success: Mapping[str, object], requested_policy: str
) -> None:
    if requested_policy not in DIRECTORY_SYNC_POLICIES:
        raise ValueError("requested directory-sync policy differs")
    if success["directory_sync_policy"] != requested_policy:
        raise ValueError(
            "published shard durability policy differs from resumed run"
        )
    outcomes = success["directory_sync_outcomes"]
    assert isinstance(outcomes, dict)
    if requested_policy == "require" and any(
        observation != "supported" for observation in outcomes.values()
    ):
        raise ValueError("required resume encountered unsupported directory sync")


def publish_shard(
    artifact: Path,
    output_root: Path,
    plan_sha256: str,
    shard: Mapping[str, object],
    elapsed: float,
    peak_rss_kib: int,
    directory_sync_policy: str,
    guest_page_size: int,
) -> Mapping[str, object]:
    shard_id = str(shard["shard_id"])
    final = output_root / "shards" / shard_id
    if final.exists():
        existing = verify_published_shard(
            final, plan_sha256, shard, guest_page_size
        )
        require_resume_durability(existing, directory_sync_policy)
        return existing
    staging_root = output_root / ".staging"
    staging_root.mkdir(exist_ok=True)
    stage = staging_root / f"{shard_id}.{uuid.uuid4().hex}.partial"
    try:
        manifest = _artifact_manifest(artifact)
        atomic_write(
            artifact / "artifact-manifest.json",
            (canonical_json(manifest) + "\n").encode("utf-8"),
            directory_sync_policy,
        )
        _copy_artifact(artifact, stage)
        copied_manifest = read_canonical_json(stage / "artifact-manifest.json")
        _verify_artifact(stage, copied_manifest)
        semantic_identity = validate_published_probe_semantics(
            stage, shard, guest_page_size
        )
        manifest_identity = file_identity(stage / "artifact-manifest.json")
        files = copied_manifest["files"]
        assert isinstance(files, list)
        (output_root / "shards").mkdir(exist_ok=True)
        campaign_root_sync = _fsync_directory(
            output_root, directory_sync_policy
        )
        stage_sync = _fsync_directory_tree(stage, directory_sync_policy)
        parent_sync = _fsync_directory(
            output_root / "shards", directory_sync_policy
        )
        success = {
            "schema": SUCCESS_SCHEMA,
            "campaign_plan_sha256": plan_sha256,
            "shard_id": shard_id,
            "contract_count": shard["contract_count"],
            "input_contract_ids_sha256": shard["input_contract_ids_sha256"],
            "artifact_manifest_sha256": manifest_identity["sha256"],
            "artifact_file_count": len(files),
            "artifact_bytes": sum(int(item["size"]) for item in files),
            "elapsed_seconds": round(elapsed, 6),
            "peak_rss_kib": peak_rss_kib,
            "directory_sync_policy": directory_sync_policy,
            "directory_sync_outcomes": {
                "artifact_stage": stage_sync,
                "campaign_root": campaign_root_sync,
                "shards_parent": parent_sync,
            },
            **semantic_identity,
        }
        success_sync = atomic_write(
            stage / "SUCCESS.json",
            (canonical_json(success) + "\n").encode("utf-8"),
            directory_sync_policy,
        )
        final_stage_sync = _fsync_directory_tree(
            stage, directory_sync_policy
        )
        if success_sync != stage_sync or final_stage_sync != stage_sync:
            raise ValueError("artifact-stage directory sync outcome changed")
        if final.exists():
            raise ValueError(f"published shard appeared concurrently: {final}")
        os.rename(stage, final)
        final_parent_sync = _fsync_directory(final.parent, directory_sync_policy)
        if final_parent_sync != parent_sync:
            raise ValueError("shards-parent directory sync outcome changed")
        return verify_published_shard(
            final, plan_sha256, shard, guest_page_size
        )
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def _probe_argv(
    plan: Mapping[str, object], shard: Mapping[str, object], artifact: Path
) -> list[str]:
    probe = plan["probe"]
    assert isinstance(probe, dict)
    source = shard["input"]
    assert isinstance(source, dict)
    return [
        str(probe["python"]),
        *ISOLATED_PYTHON_FLAGS,
        str(probe["path"]),
        "--count", str(shard["contract_count"]),
        "--input-jsonl", str(source["path"]),
        "--architecture-root", str(probe["architecture_root"]),
        "--aslref", str(probe["aslref"]),
        "--bundler", str(probe["bundler"]),
        "--features-json", str(probe["features_json"]),
        "--closure-capture-directory", str(probe["closure_capture_directory"]),
        "--required-variants", str(probe["required_variants"]),
        "--interface", str(probe["interface"]),
        "--observer", str(probe["observer"]),
        "--max-contracts", str(MAX_SHARD_CONTRACTS),
        "--max-encoded-bytes", str(probe["max_encoded_bytes"]),
        "--max-artifact-bytes", str(probe["max_artifact_bytes"]),
        "--max-artifact-files", str(probe["max_artifact_files"]),
        "--max-log-bytes", str(probe["max_log_bytes"]),
        "--minimum-generated-free-gib", str(PROBE_MINIMUM_GENERATED_FREE_GIB),
        "--timeout-seconds", str(probe["timeout_seconds"]),
        "--max-rss-kib", str(probe["max_rss_kib"]),
        "--generated-directory", str(artifact),
    ]


def sanitized_probe_environment() -> dict[str, str]:
    return {
        name: value
        for name, value in os.environ.items()
        if not name.upper().startswith("PYTHON")
    }


def _run_process_with_capped_logs(
    argv: Sequence[str],
    stdout_path: Path,
    stderr_path: Path,
    environment: Mapping[str, str],
    timeout_seconds: int,
    maximum_log_bytes: int,
    cancel_event: threading.Event | None,
    maximum_workspace_bytes: int | None = None,
    maximum_workspace_files: int = DEFAULT_MAX_ARTIFACT_FILES + 2,
) -> int:
    _strict_integer(timeout_seconds, "campaign probe timeout", 1)
    _strict_integer(maximum_log_bytes, "campaign probe log cap", 1)
    _strict_integer(
        maximum_workspace_files, "campaign probe workspace file cap", 1
    )
    workspace_bytes = (
        maximum_log_bytes
        if maximum_workspace_bytes is None
        else _strict_integer(
            maximum_workspace_bytes, "campaign probe workspace byte cap", 1
        )
    )
    if workspace_bytes < maximum_log_bytes:
        raise ValueError("campaign probe workspace cap is below its log cap")
    stdout_parent = stdout_path.parent.resolve(strict=True)
    if not paths_alias(stdout_parent, stderr_path.parent.resolve(strict=True)):
        raise ValueError("campaign probe logs do not share one workspace")
    if stdout_path.exists() or stderr_path.exists():
        raise ValueError("campaign probe log target already exists")
    limits = GenerationLimits(
        free_space_reserve_bytes=1,
        max_workspace_bytes=workspace_bytes,
        max_artifact_bytes=workspace_bytes,
        max_log_bytes=maximum_log_bytes,
        max_workspace_files=maximum_workspace_files,
    )
    with BoundedWorkspace.adopt(
        stdout_parent,
        source_root=Path(__file__).resolve().parents[3],
        publication_targets=(),
        limits=limits,
    ) as workspace:
        completed = run_bounded(
            argv,
            workspace=workspace,
            timeout_seconds=timeout_seconds,
            environment=environment,
            cancel_event=cancel_event,
        )
        workspace.write_text(stdout_path, completed.stdout)
        workspace.write_text(stderr_path, completed.stderr)
        return int(completed.returncode)


def require_artifact_limits(
    artifact: Path, plan: Mapping[str, object]
) -> tuple[int, int]:
    probe = plan["probe"]
    assert isinstance(probe, Mapping)
    maximum_bytes = _strict_integer(
        probe["max_artifact_bytes"], "campaign artifact byte cap", 1
    )
    maximum_files = _strict_integer(
        probe["max_artifact_files"], "campaign artifact file cap", 1
    )
    files = [path for path in artifact.rglob("*") if path.is_file()]
    byte_count = sum(path.stat().st_size for path in files)
    if len(files) > maximum_files:
        raise ValueError(
            f"campaign artifact has {len(files)} files, cap is {maximum_files}"
        )
    if byte_count > maximum_bytes:
        raise ValueError(
            f"campaign artifact has {byte_count} bytes, cap is {maximum_bytes}"
        )
    for path in files:
        relative = path.relative_to(artifact).as_posix()
        if (
            len(relative.encode("utf-8"))
            > CAMPAIGN_ARTIFACT_RELATIVE_PATH_BYTES
            or re.fullmatch(r"[A-Za-z0-9._/-]+", relative) is None
        ):
            raise ValueError("campaign artifact path is outside the safe bound")
    return len(files), byte_count


def _require_not_cancelled(
    cancel_event: threading.Event | None, shard_id: object
) -> None:
    if cancel_event is not None and cancel_event.is_set():
        raise ValueError(f"{shard_id}: campaign shard was cancelled")


def run_one_shard(
    plan: Mapping[str, object],
    plan_sha256: str,
    shard: Mapping[str, object],
    output_root: Path,
    local_temp_root: Path,
    directory_sync_policy: str,
    cancel_event: threading.Event | None = None,
) -> Mapping[str, object]:
    _require_not_cancelled(cancel_event, shard["shard_id"])
    final = output_root / "shards" / str(shard["shard_id"])
    if final.exists():
        existing = verify_published_shard(
            final, plan_sha256, shard, int(plan["guest_page_size"])
        )
        require_resume_durability(existing, directory_sync_policy)
        return existing
    source = shard["input"]
    assert isinstance(source, dict)
    path = Path(str(source["path"]))
    identity = file_identity(path)
    if identity["size"] != source["size"] or identity["sha256"] != source["sha256"]:
        raise ValueError(f"{shard['shard_id']}: frozen shard input changed")
    count, digest = scan_shard_input(
        path, guest_page_size=int(plan["guest_page_size"])
    )
    if count != shard["contract_count"] or digest != shard["input_contract_ids_sha256"]:
        raise ValueError(f"{shard['shard_id']}: shard input contract identity changed")
    _require_not_cancelled(cancel_event, shard["shard_id"])
    temporary = Path(
        tempfile.mkdtemp(
            prefix=f"{shard['shard_id']}.",
            suffix=".partial",
            dir=local_temp_root,
        )
    )
    try:
        artifact = temporary / "artifact"
        stdout_path = temporary / "probe.stdout"
        stderr_path = temporary / "probe.stderr"
        argv = _probe_argv(plan, shard, artifact)
        started = time.perf_counter()
        probe = plan["probe"]
        assert isinstance(probe, Mapping)
        returncode = _run_process_with_capped_logs(
            argv,
            stdout_path,
            stderr_path,
            sanitized_probe_environment(),
            int(probe["timeout_seconds"]) + 120,
            int(probe["max_log_bytes"]),
            cancel_event,
            (
                int(probe["max_artifact_bytes"])
                + int(probe["max_log_bytes"])
            ),
            int(probe["max_artifact_files"]) + 2,
        )
        elapsed = time.perf_counter() - started
        if returncode != 0:
            tail = stderr_path.read_text(encoding="utf-8", errors="replace")[-3000:]
            raise ValueError(
                f"{shard['shard_id']}: probe exited {returncode}: {tail}"
            )
        os.rename(stdout_path, artifact / "campaign-probe.stdout")
        os.rename(stderr_path, artifact / "campaign-probe.stderr")
        report = read_canonical_json(artifact / "batch-report.json")
        result_path = artifact / "aslref-result.json"
        result = json.loads(
            result_path.read_text(encoding="utf-8"),
            parse_constant=lambda constant: (_ for _ in ()).throw(
                ValueError(
                    f"{result_path}: nonfinite JSON constant {constant}"
                )
            ),
        )
        if (
            report.get("schema") != PROBE_FORMAT
            or report.get("selection") != "explicit-unique-campaign-shard"
            or report.get("contracts") != shard["contract_count"]
            or report.get("unique_input_contracts") != shard["contract_count"]
            or report.get("promotable") is not False
            or not isinstance(result, dict)
            or result.get("passed") is not True
            or result.get("accepted") is not False
            or result.get("returncode") != 0
        ):
            raise ValueError(f"{shard['shard_id']}: probe success contract differs")
        require_artifact_limits(artifact, plan)
        _require_not_cancelled(cancel_event, shard["shard_id"])
        after = file_identity(path)
        if after != identity:
            raise ValueError(f"{shard['shard_id']}: shard input changed during execution")
        return publish_shard(
            artifact,
            output_root,
            plan_sha256,
            shard,
            elapsed,
            int(report["peak_rss_kib"]),
            directory_sync_policy,
            int(plan["guest_page_size"]),
        )
    finally:
        shutil.rmtree(temporary)


def run_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--local-temp-root", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--minimum-output-free-gib", type=float, default=20.0)
    parser.add_argument("--minimum-local-free-gib", type=float, default=5.0)
    parser.add_argument(
        "--maximum-authority-database-bytes",
        type=int,
        default=DEFAULT_MAX_AUTHORITY_DATABASE_BYTES,
    )
    parser.add_argument(
        "--maximum-authority-merge-scratch-bytes",
        type=int,
        default=DEFAULT_MAX_AUTHORITY_MERGE_SCRATCH_BYTES,
    )
    parser.add_argument(
        "--directory-sync-policy", choices=("require", "record-unsupported"),
        default="require",
    )


def require_root(path: Path, label: str) -> Path:
    if not path.is_absolute() or path.is_symlink() or not path.is_dir():
        raise ValueError(f"{label} must be an existing absolute nonsymlink directory")
    return path.resolve(strict=True)


def _path_contains(parent: Path, child: Path) -> bool:
    return paths_alias(parent, child) or path_is_ancestor(parent, child)


def validate_run_root_isolation(
    plan_path: Path,
    plan: Mapping[str, object],
    output_root: Path,
    local_temp_root: Path | None,
) -> None:
    protected: list[tuple[Path, str]] = [(plan_path, "file")]
    for entry in plan["source_freeze"]:  # type: ignore[index]
        assert isinstance(entry, Mapping)
        protected.append((Path(str(entry["path"])), str(entry["kind"])))
    for shard in plan["shards"]:  # type: ignore[index]
        assert isinstance(shard, Mapping)
        source = shard["input"]
        assert isinstance(source, Mapping)
        protected.append((Path(str(source["path"])), "file"))
    for path, _kind in protected:
        if _path_contains(output_root, path) or _path_contains(path, output_root):
            raise ValueError(
                "campaign output root aliases a frozen plan/source/input path: "
                f"{path}"
            )
    if local_temp_root is None:
        return
    if _path_contains(output_root, local_temp_root) or _path_contains(
        local_temp_root, output_root
    ):
        raise ValueError("campaign output and local temporary roots overlap")
    for path, _kind in protected:
        if _path_contains(path, local_temp_root) or _path_contains(
            local_temp_root, path
        ):
            raise ValueError(
                "campaign local temporary root aliases a frozen plan/source/input: "
                f"{path}"
            )


CAMPAIGN_ARTIFACT_RELATIVE_PATH_BYTES = 4096
CAMPAIGN_METADATA_FIXED_HEADROOM_BYTES = 1024 * 1024
CAMPAIGN_METADATA_BYTES_PER_ARTIFACT_FILE = (
    CAMPAIGN_ARTIFACT_RELATIVE_PATH_BYTES + 256
)


def require_free_space(
    path: Path,
    gib: float,
    label: str,
    *,
    headroom_bytes: int = 0,
) -> None:
    if (
        isinstance(gib, bool)
        or not isinstance(gib, (int, float))
        or not math.isfinite(gib)
        or gib < 0
    ):
        raise ValueError(f"{label} free-space reserve is invalid")
    if isinstance(headroom_bytes, bool) or not isinstance(headroom_bytes, int):
        raise ValueError(f"{label} free-space headroom is not an integer")
    if headroom_bytes < 0:
        raise ValueError(f"{label} free-space headroom is negative")
    required = int(gib * 1024 * 1024 * 1024) + headroom_bytes
    free = shutil.disk_usage(path).free
    if free < required:
        raise ValueError(f"{label} free space {free} is below reserve {required}")


def campaign_shard_disk_headroom(
    plan: Mapping[str, object], target: str
) -> int:
    probe = plan["probe"]
    assert isinstance(probe, Mapping)
    maximum_artifact_bytes = _strict_integer(
        probe["max_artifact_bytes"], "campaign artifact byte cap", 1
    )
    maximum_log_bytes = _strict_integer(
        probe["max_log_bytes"], "campaign probe log cap", 1
    )
    maximum_artifact_files = _strict_integer(
        probe["max_artifact_files"], "campaign artifact file cap", 1
    )
    if target == "output":
        return (
            maximum_artifact_bytes
            + CAMPAIGN_METADATA_FIXED_HEADROOM_BYTES
            + maximum_artifact_files
            * CAMPAIGN_METADATA_BYTES_PER_ARTIFACT_FILE
        )
    if target == "local":
        return maximum_artifact_bytes + maximum_log_bytes
    raise ValueError("campaign disk-headroom target differs")


def require_campaign_capacity(
    plan: Mapping[str, object],
    output_root: Path,
    local_temp_root: Path,
    minimum_output_free_gib: float,
    minimum_local_free_gib: float,
    active_shards: int,
) -> None:
    if isinstance(active_shards, bool) or not isinstance(active_shards, int):
        raise ValueError("active campaign shard count is not an integer")
    if active_shards < 0 or active_shards > 4:
        raise ValueError("active campaign shard count is outside 0..4")
    output_headroom = (
        campaign_shard_disk_headroom(plan, "output") * active_shards
    )
    local_headroom = (
        campaign_shard_disk_headroom(plan, "local") * active_shards
    )
    require_free_space(
        output_root,
        minimum_output_free_gib,
        "output",
        headroom_bytes=output_headroom,
    )
    require_free_space(
        local_temp_root,
        minimum_local_free_gib,
        "local",
        headroom_bytes=local_headroom,
    )


LOCAL_WORKSPACE = re.compile(r"\.a64-campaign-[0-9a-f]{32}\Z")
LOCAL_SHARD_STAGE = re.compile(
    r"shard-[0-9]{8}\.[A-Za-z0-9_-]+\.partial\Z"
)


@contextmanager
def campaign_local_workspace(
    local_temp_root: Path,
    output_root: Path,
    plan_sha256: str,
) -> Iterator[Path]:
    owner = hashlib.sha256(
        (plan_sha256 + "\0" + str(output_root)).encode("utf-8")
    ).hexdigest()[:32]
    workspace = local_temp_root / f".a64-campaign-{owner}"
    if LOCAL_WORKSPACE.fullmatch(workspace.name) is None:
        raise ValueError("campaign local workspace name differs")
    workspace.mkdir(exist_ok=True)
    if workspace.is_symlink() or not workspace.is_dir():
        raise ValueError("campaign local workspace is not a nonsymlink directory")

    def clean() -> None:
        for entry in list(workspace.iterdir()):
            if (
                LOCAL_SHARD_STAGE.fullmatch(entry.name) is None
                or entry.is_symlink()
                or not entry.is_dir()
            ):
                raise ValueError(
                    f"campaign local workspace contains an unsafe entry: "
                    f"{entry.name}"
                )
            shutil.rmtree(entry)

    clean()
    try:
        yield workspace
    finally:
        clean()
        workspace.rmdir()


@contextmanager
def campaign_lock(
    output_root: Path,
    directory_sync_policy: str,
    *,
    create: bool = True,
):
    path = output_root / "campaign.lock"
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise ValueError("campaign lock is not a nonsymlink file")
    if not create and not path.is_file():
        raise ValueError("campaign lock is missing")
    created = not path.exists()
    flags = (
        os.O_RDWR | os.O_CREAT
        if create
        else os.O_RDONLY
    ) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags, 0o600)
    try:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise ValueError("another campaign runner holds the output-root lock") from error
        if create:
            os.fsync(descriptor)
        if create and created:
            _fsync_directory(output_root, directory_sync_policy)
        yield
    finally:
        fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)


def validate_campaign_report(
    report: Mapping[str, object],
    plan: Mapping[str, object],
    plan_sha256: str,
    results: Sequence[Mapping[str, object]],
) -> Mapping[str, object]:
    expected_keys = {
        "schema", "campaign_id", "campaign_plan_sha256", "shards",
        "contracts", "workers", "invocation_elapsed_seconds",
        "published_this_run_contracts_per_second",
        "preexisting_shard_count", "preexisting_contract_count",
        "preexisting_shard_ids", "preexisting_shard_ids_sha256",
        "published_this_run_shard_count", "published_this_run_contract_count",
        "published_this_run_shard_ids",
        "published_this_run_shard_ids_sha256",
        "sum_shard_peak_rss_kib", "maximum_shard_peak_rss_kib",
        "published_artifact_bytes", "published_file_count",
        "directory_sync_policy", "failures", "semantic_promotion",
        "directory_sync_observations", "campaign_directory_sync_outcome",
        "published_probe_result_binding_count",
        "published_probe_result_bindings_sha256",
    }
    if set(report) != expected_keys or report.get("schema") != REPORT_SCHEMA:
        raise ValueError("campaign report schema/fields differ")
    shards = plan["shards"]
    assert isinstance(shards, list)
    result_binding_count, result_binding_digest = _digest_tuples(
        (
            str(shard["shard_id"]),
            str(result["probe_report_sha256"]),
            str(result["probe_result_sha256"]),
            str(result["input_results_sha256"]),
            str(result["artifact_manifest_sha256"]),
        )
        for shard, result in zip(shards, results, strict=True)
    )
    fixed = {
        "campaign_id": plan["campaign_id"],
        "campaign_plan_sha256": plan_sha256,
        "shards": len(results),
        "contracts": plan["total_contracts"],
        "sum_shard_peak_rss_kib": sum(
            int(result["peak_rss_kib"]) for result in results
        ),
        "maximum_shard_peak_rss_kib": max(
            int(result["peak_rss_kib"]) for result in results
        ),
        "published_artifact_bytes": sum(
            int(result["artifact_bytes"]) for result in results
        ),
        "published_file_count": sum(
            int(result["artifact_file_count"]) + 2 for result in results
        ),
        "failures": 0,
        "semantic_promotion": "requires-exact-frozen-join",
        "published_probe_result_binding_count": result_binding_count,
        "published_probe_result_bindings_sha256": result_binding_digest,
    }
    integer_fields = {
        "shards": (1, MAX_ARTIFACT_SCALAR),
        "contracts": (1, MAX_ARTIFACT_SCALAR),
        "sum_shard_peak_rss_kib": (0, MAX_ARTIFACT_SCALAR),
        "maximum_shard_peak_rss_kib": (0, MAX_ARTIFACT_SCALAR),
        "published_artifact_bytes": (0, MAX_ARTIFACT_SCALAR),
        "published_file_count": (1, MAX_ARTIFACT_SCALAR),
        "failures": (0, 0),
        "published_probe_result_binding_count": (1, MAX_ARTIFACT_SCALAR),
        "preexisting_shard_count": (0, MAX_ARTIFACT_SCALAR),
        "preexisting_contract_count": (0, MAX_ARTIFACT_SCALAR),
        "published_this_run_shard_count": (0, MAX_ARTIFACT_SCALAR),
        "published_this_run_contract_count": (0, MAX_ARTIFACT_SCALAR),
    }
    for name, (minimum, maximum) in integer_fields.items():
        _strict_integer(report[name], f"campaign report {name}", minimum, maximum)
    for name in (
        "published_probe_result_bindings_sha256",
        "preexisting_shard_ids_sha256",
        "published_this_run_shard_ids_sha256",
    ):
        if not isinstance(report[name], str) or SHA256.fullmatch(report[name]) is None:
            raise ValueError(f"campaign report {name} differs")
    if any(report.get(name) != value for name, value in fixed.items()):
        raise ValueError("campaign report binding/counts differ")
    workers = report["workers"]
    if (
        isinstance(workers, bool)
        or not isinstance(workers, int)
        or workers <= 0
        or workers > 4
    ):
        raise ValueError("campaign report worker count differs")
    shard_by_id = {str(shard["shard_id"]): shard for shard in shards}
    accounting_sets: list[list[str]] = []
    for name in (
        "preexisting_shard_ids",
        "published_this_run_shard_ids",
    ):
        identifiers = report[name]
        if (
            not isinstance(identifiers, list)
            or any(
                not isinstance(identifier, str)
                or SHARD_ID.fullmatch(identifier) is None
                for identifier in identifiers
            )
            or identifiers != sorted(set(identifiers))
        ):
            raise ValueError(f"campaign report {name} differs")
        accounting_sets.append(identifiers)
    preexisting_ids, published_ids = accounting_sets
    if (
        set(preexisting_ids) & set(published_ids)
        or set(preexisting_ids) | set(published_ids) != set(shard_by_id)
        or report["preexisting_shard_count"] != len(preexisting_ids)
        or report["published_this_run_shard_count"] != len(published_ids)
        or report["preexisting_contract_count"]
        != sum(int(shard_by_id[name]["contract_count"]) for name in preexisting_ids)
        or report["published_this_run_contract_count"]
        != sum(int(shard_by_id[name]["contract_count"]) for name in published_ids)
    ):
        raise ValueError("campaign report invocation accounting differs")
    _, preexisting_digest = _digest_tuples(
        (name,) for name in preexisting_ids
    )
    _, published_digest = _digest_tuples(
        (name,) for name in published_ids
    )
    if (
        report["preexisting_shard_ids_sha256"] != preexisting_digest
        or report["published_this_run_shard_ids_sha256"] != published_digest
    ):
        raise ValueError("campaign report invocation ID digest differs")
    elapsed = report["invocation_elapsed_seconds"]
    throughput = report["published_this_run_contracts_per_second"]
    newly_published = int(report["published_this_run_contract_count"])
    if (
        isinstance(elapsed, bool)
        or not isinstance(elapsed, (int, float))
        or not math.isfinite(elapsed)
        or elapsed <= 0
        or isinstance(throughput, bool)
        or not isinstance(throughput, (int, float))
        or not math.isfinite(throughput)
        or throughput < 0
        or throughput != (
            round(newly_published / elapsed, 6) if newly_published else 0
        )
    ):
        raise ValueError("campaign report timing/throughput differs")
    policies = {str(result["directory_sync_policy"]) for result in results}
    if (
        len(policies) != 1
        or report["directory_sync_policy"] not in policies
        or report["directory_sync_policy"] not in DIRECTORY_SYNC_POLICIES
    ):
        raise ValueError("campaign report durability policy differs")
    campaign_sync = _validate_sync_observation(
        report["campaign_directory_sync_outcome"],
        "campaign directory-sync outcome",
    )
    shard_syncs = {
        str(observation)
        for result in results
        for observation in result["directory_sync_outcomes"].values()  # type: ignore[union-attr]
    }
    observations = report["directory_sync_observations"]
    if (
        not isinstance(observations, list)
        or observations != sorted(shard_syncs | {campaign_sync})
    ):
        raise ValueError("campaign directory-sync observations differ")
    if report["directory_sync_policy"] == "require" and observations != ["supported"]:
        raise ValueError("campaign required directory sync was unsupported")
    return report


def _verified_results(
    output_root: Path, plan: Mapping[str, object], plan_sha256: str
) -> list[Mapping[str, object]]:
    shards = plan["shards"]
    assert isinstance(shards, list)
    shard_root = output_root / "shards"
    staging_root = output_root / ".staging"
    if (
        shard_root.is_symlink()
        or not shard_root.is_dir()
        or staging_root.is_symlink()
        or not staging_root.is_dir()
    ):
        raise ValueError("campaign shard/staging roots are not nonsymlink directories")
    expected_names = {str(shard["shard_id"]) for shard in shards}
    actual_names = {path.name for path in shard_root.iterdir()}
    if actual_names != expected_names:
        raise ValueError("published shard directory set differs from campaign plan")
    if any(path.is_symlink() or not path.is_dir() for path in shard_root.iterdir()):
        raise ValueError("published shard set contains a non-directory or symlink")
    if any(staging_root.iterdir()):
        raise ValueError("campaign staging directory is not empty")
    return [
        verify_published_shard(
            shard_root / str(shard["shard_id"]),
            plan_sha256,
            shard,
            int(plan["guest_page_size"]),
        )
        for shard in shards
    ]


def preflight_campaign_output_root(output_root: Path) -> None:
    metadata = {"campaign-plan.json", "campaign-report.json", "campaign.lock"}
    directories = {"shards", ".staging"}
    unrelated = sorted(
        path.name
        for path in output_root.iterdir()
        if path.name not in metadata | directories
    )
    if unrelated:
        raise ValueError(
            "campaign output root contains unrelated entries: "
            + ",".join(unrelated[:3])
        )
    for name in metadata:
        path = output_root / name
        if path.exists() and (path.is_symlink() or not path.is_file()):
            raise ValueError(f"campaign output metadata target is unsafe: {name}")
    for name in directories:
        path = output_root / name
        if path.exists() and (path.is_symlink() or not path.is_dir()):
            raise ValueError(f"campaign output directory target is unsafe: {name}")
    shard_root = output_root / "shards"
    if shard_root.is_dir():
        for entry in shard_root.iterdir():
            if (
                SHARD_ID.fullmatch(entry.name) is None
                or entry.is_symlink()
                or not entry.is_dir()
            ):
                raise ValueError(
                    f"campaign output contains an unsafe shard: {entry.name}"
                )
    staging = output_root / ".staging"
    stale_pattern = re.compile(r"shard-[0-9]{8}\.[0-9a-f]{32}\.partial\Z")
    if staging.is_dir():
        for entry in staging.iterdir():
            if (
                stale_pattern.fullmatch(entry.name) is None
                or entry.is_symlink()
                or not entry.is_dir()
            ):
                raise ValueError(
                    f"campaign staging contains an unsafe entry: {entry.name}"
                )


def pilot_campaign(options: argparse.Namespace) -> Mapping[str, object]:
    if options.workers != 4:
        raise ValueError("campaign pilot requires exactly four workers")
    plan_path = require_regular_file(options.plan, "campaign plan")
    plan_bytes = plan_path.read_bytes()
    plan = validate_plan(parse_canonical_json(plan_bytes, str(plan_path)))
    shards = plan["shards"]
    assert isinstance(shards, list)
    if len(shards) <= 4 or any(
        shard["contract_count"] != MAX_SHARD_CONTRACTS
        for shard in shards[:4]
    ):
        raise ValueError(
            "campaign pilot requires the first four of a larger full plan "
            "to contain exactly 1024 contracts each"
        )
    output_root = require_root(options.output_root, "output root")
    local_temp_root = require_root(options.local_temp_root, "local temp root")
    validate_run_root_isolation(
        plan_path, plan, output_root, local_temp_root
    )
    preflight_campaign_output_root(output_root)
    require_campaign_capacity(
        plan,
        output_root,
        local_temp_root,
        options.minimum_output_free_gib,
        options.minimum_local_free_gib,
        0,
    )
    with campaign_lock(output_root, options.directory_sync_policy):
        return _pilot_campaign_locked(
            options,
            plan_path,
            plan_bytes,
            plan,
            output_root,
            local_temp_root,
        )


def _pilot_campaign_locked(
    options: argparse.Namespace,
    plan_path: Path,
    plan_bytes: bytes,
    plan: Mapping[str, object],
    output_root: Path,
    local_temp_root: Path,
) -> Mapping[str, object]:
    preflight_campaign_output_root(output_root)
    require_plan_snapshot(plan_path, plan_bytes)
    allowed = {"campaign-plan.json", "campaign.lock", "shards", ".staging"}
    unrelated = sorted(
        path.name for path in output_root.iterdir() if path.name not in allowed
    )
    if unrelated:
        raise ValueError(
            "pilot output root is not dedicated/incomplete: "
            + ",".join(unrelated[:3])
        )
    if (output_root / "campaign-report.json").exists():
        raise ValueError("pilot must not run against a completed campaign report")
    plan_sha256 = hashlib.sha256(plan_bytes).hexdigest()
    frozen_plan = output_root / "campaign-plan.json"
    if frozen_plan.exists():
        if frozen_plan.is_symlink() or not frozen_plan.is_file():
            raise ValueError("frozen campaign plan is not a nonsymlink file")
        if frozen_plan.read_bytes() != plan_bytes:
            raise ValueError("pilot output root is bound to another campaign plan")
    else:
        atomic_write(frozen_plan, plan_bytes, options.directory_sync_policy)
    shard_root = output_root / "shards"
    staging = output_root / ".staging"
    shard_root.mkdir(exist_ok=True)
    staging.mkdir(exist_ok=True)
    _fsync_directory(output_root, options.directory_sync_policy)
    for path in (shard_root, staging):
        if path.is_symlink() or not path.is_dir():
            raise ValueError("pilot shard/staging root is not a directory")
    stale_pattern = re.compile(r"shard-[0-9]{8}\.[0-9a-f]{32}\.partial\Z")
    for path in list(staging.iterdir()):
        if (
            stale_pattern.fullmatch(path.name) is None
            or path.is_symlink()
            or not path.is_dir()
        ):
            raise ValueError(f"pilot staging contains an unsafe entry: {path.name}")
        shutil.rmtree(path)
    _fsync_directory(staging, options.directory_sync_policy)
    verify_source_freeze(plan)
    verify_authoritative_plan(
        plan,
        local_temp_root=local_temp_root,
        minimum_local_free_gib=options.minimum_local_free_gib,
        maximum_database_bytes=options.maximum_authority_database_bytes,
        maximum_merge_scratch_bytes=(
            options.maximum_authority_merge_scratch_bytes
        ),
    )
    shards = plan["shards"]
    assert isinstance(shards, list)
    selected = shards[:4]
    selected_by_id = {str(shard["shard_id"]): shard for shard in selected}
    preexisting = {path.name for path in shard_root.iterdir()}
    if not preexisting <= set(selected_by_id):
        raise ValueError("pilot output contains a shard outside its first four")
    before_success: dict[str, bytes] = {}
    for name in sorted(preexisting):
        result = verify_published_shard(
            shard_root / name,
            plan_sha256,
            selected_by_id[name],
            int(plan["guest_page_size"]),
        )
        require_resume_durability(result, options.directory_sync_policy)
        before_success[name] = (shard_root / name / "SUCCESS.json").read_bytes()
    missing = [
        shard for shard in selected
        if str(shard["shard_id"]) not in preexisting
    ]
    started = time.perf_counter()
    cancel_event = threading.Event()
    with campaign_local_workspace(
        local_temp_root, output_root, plan_sha256
    ) as local_workspace:
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            pending: set[concurrent.futures.Future[Mapping[str, object]]] = set()
            try:
                for shard in missing:
                    require_campaign_capacity(
                        plan,
                        output_root,
                        local_temp_root,
                        options.minimum_output_free_gib,
                        options.minimum_local_free_gib,
                        len(pending) + 1,
                    )
                    pending.add(
                        pool.submit(
                            run_one_shard,
                            plan,
                            plan_sha256,
                            shard,
                            output_root,
                            local_workspace,
                            options.directory_sync_policy,
                            cancel_event,
                        )
                    )
                while pending:
                    done, _ = concurrent.futures.wait(
                        pending,
                        return_when=concurrent.futures.FIRST_COMPLETED,
                    )
                    for future in done:
                        pending.remove(future)
                    for future in done:
                        future.result()
                    for _future in done:
                        require_campaign_capacity(
                            plan,
                            output_root,
                            local_temp_root,
                            options.minimum_output_free_gib,
                            options.minimum_local_free_gib,
                            len(pending),
                        )
            except BaseException:
                cancel_event.set()
                for future in pending:
                    future.cancel()
                raise
    elapsed = round(time.perf_counter() - started, 6)
    if {path.name for path in shard_root.iterdir()} != set(selected_by_id):
        raise ValueError("pilot did not publish exactly its first four shards")
    if any(staging.iterdir()):
        raise ValueError("pilot staging directory is not empty")
    results = []
    for shard in selected:
        name = str(shard["shard_id"])
        results.append(
            verify_published_shard(
                shard_root / name,
                plan_sha256,
                shard,
                int(plan["guest_page_size"]),
            )
        )
        if name in before_success and (
            shard_root / name / "SUCCESS.json"
        ).read_bytes() != before_success[name]:
            raise ValueError("pilot resume changed an existing SUCCESS artifact")
    result_count, result_digest = _digest_tuples(
        (
            str(shard["shard_id"]),
            str(result["probe_report_sha256"]),
            str(result["probe_result_sha256"]),
            str(result["input_results_sha256"]),
            str(result["artifact_manifest_sha256"]),
        )
        for shard, result in zip(selected, results, strict=True)
    )
    verify_source_freeze(plan)
    require_plan_snapshot(plan_path, plan_bytes)
    return {
        "schema": PILOT_SCHEMA,
        "campaign_id": plan["campaign_id"],
        "campaign_plan_sha256": plan_sha256,
        "workers": 4,
        "shards": 4,
        "contracts": 4 * MAX_SHARD_CONTRACTS,
        "preexisting_shards": len(preexisting),
        "published_this_run_shards": 4 - len(preexisting),
        "invocation_elapsed_seconds": elapsed,
        "published_probe_result_binding_count": result_count,
        "published_probe_result_bindings_sha256": result_digest,
        "campaign_report_published": False,
        "semantic_promotion": "forbidden-capacity-pilot",
    }


def run_campaign(options: argparse.Namespace) -> Mapping[str, object]:
    if options.workers <= 0 or options.workers > 4:
        raise ValueError("workers must be in 1..4 pending higher-concurrency evidence")
    plan_path = require_regular_file(options.plan, "campaign plan")
    plan_bytes = plan_path.read_bytes()
    plan = validate_plan(parse_canonical_json(plan_bytes, str(plan_path)))
    output_root = require_root(options.output_root, "output root")
    local_temp_root = require_root(options.local_temp_root, "local temp root")
    validate_run_root_isolation(
        plan_path, plan, output_root, local_temp_root
    )
    preflight_campaign_output_root(output_root)
    require_campaign_capacity(
        plan,
        output_root,
        local_temp_root,
        options.minimum_output_free_gib,
        options.minimum_local_free_gib,
        0,
    )
    with campaign_lock(output_root, options.directory_sync_policy):
        return _run_campaign_locked(
            options,
            plan_path,
            plan_bytes,
            plan,
            output_root,
            local_temp_root,
        )


def _run_campaign_locked(
    options: argparse.Namespace,
    plan_path: Path,
    plan_bytes: bytes,
    plan: Mapping[str, object],
    output_root: Path,
    local_temp_root: Path,
) -> Mapping[str, object]:
    preflight_campaign_output_root(output_root)
    require_plan_snapshot(plan_path, plan_bytes)
    allowed = {
        "campaign-plan.json", "campaign-report.json", "campaign.lock",
        "shards", ".staging",
    }
    unrelated = sorted(
        path.name for path in output_root.iterdir() if path.name not in allowed
    )
    if unrelated:
        raise ValueError("output root is not dedicated: " + ",".join(unrelated[:3]))
    plan_sha256 = hashlib.sha256(plan_bytes).hexdigest()
    frozen_plan = output_root / "campaign-plan.json"
    if frozen_plan.exists():
        if frozen_plan.is_symlink() or not frozen_plan.is_file():
            raise ValueError("frozen campaign plan is not a nonsymlink file")
        if frozen_plan.read_bytes() != plan_bytes:
            raise ValueError("output root is bound to another campaign plan")
    else:
        atomic_write(frozen_plan, plan_bytes, options.directory_sync_policy)
    (output_root / "shards").mkdir(exist_ok=True)
    (output_root / ".staging").mkdir(exist_ok=True)
    _fsync_directory(output_root, options.directory_sync_policy)
    for path in (output_root / "shards", output_root / ".staging"):
        if path.is_symlink() or not path.is_dir():
            raise ValueError("campaign shard/staging root is not a directory")
    staging = output_root / ".staging"
    stale_pattern = re.compile(
        r"shard-[0-9]{8}\.[0-9a-f]{32}\.partial\Z"
    )
    for path in list(staging.iterdir()):
        if (
            stale_pattern.fullmatch(path.name) is None
            or path.is_symlink()
            or not path.is_dir()
        ):
            raise ValueError(f"campaign staging contains an unsafe entry: {path.name}")
        shutil.rmtree(path)
    _fsync_directory(staging, options.directory_sync_policy)
    verify_source_freeze(plan)
    verify_authoritative_plan(
        plan,
        local_temp_root=local_temp_root,
        minimum_local_free_gib=options.minimum_local_free_gib,
        maximum_database_bytes=options.maximum_authority_database_bytes,
        maximum_merge_scratch_bytes=(
            options.maximum_authority_merge_scratch_bytes
        ),
    )
    shards = plan["shards"]
    assert isinstance(shards, list)
    shard_by_id = {str(shard["shard_id"]): shard for shard in shards}
    shard_root = output_root / "shards"
    preexisting_names = {path.name for path in shard_root.iterdir()}
    if not preexisting_names <= set(shard_by_id):
        raise ValueError("output root contains an unrelated published shard")
    for name in sorted(preexisting_names):
        existing = verify_published_shard(
            shard_root / name,
            plan_sha256,
            shard_by_id[name],
            int(plan["guest_page_size"]),
        )
        require_resume_durability(existing, options.directory_sync_policy)
    preexisting_contracts = sum(
        int(shard_by_id[name]["contract_count"])
        for name in preexisting_names
    )
    started = time.perf_counter()
    iterator = iter(
        shard for shard in shards
        if str(shard["shard_id"]) not in preexisting_names
    )
    cancel_event = threading.Event()
    with campaign_local_workspace(
        local_temp_root, output_root, plan_sha256
    ) as local_workspace:
        with concurrent.futures.ThreadPoolExecutor(
            max_workers=options.workers
        ) as pool:
            pending: dict[
                concurrent.futures.Future[Mapping[str, object]],
                Mapping[str, object],
            ] = {}
            try:
                for _ in range(options.workers):
                    try:
                        shard = next(iterator)
                    except StopIteration:
                        break
                    require_campaign_capacity(
                        plan,
                        output_root,
                        local_temp_root,
                        options.minimum_output_free_gib,
                        options.minimum_local_free_gib,
                        len(pending) + 1,
                    )
                    pending[pool.submit(
                        run_one_shard, plan, plan_sha256, shard, output_root,
                        local_workspace, options.directory_sync_policy,
                        cancel_event,
                    )] = shard
                while pending:
                    done, _ = concurrent.futures.wait(
                        pending, return_when=concurrent.futures.FIRST_COMPLETED
                    )
                    for future in done:
                        pending.pop(future)
                    # Consume the entire completed batch before refilling.  A
                    # later failure in the same batch must prevent all new work.
                    for future in done:
                        future.result()
                    for _future in done:
                        require_campaign_capacity(
                            plan,
                            output_root,
                            local_temp_root,
                            options.minimum_output_free_gib,
                            options.minimum_local_free_gib,
                            len(pending),
                        )
                        try:
                            shard = next(iterator)
                        except StopIteration:
                            continue
                        require_campaign_capacity(
                            plan,
                            output_root,
                            local_temp_root,
                            options.minimum_output_free_gib,
                            options.minimum_local_free_gib,
                            len(pending) + 1,
                        )
                        pending[pool.submit(
                            run_one_shard,
                            plan,
                            plan_sha256,
                            shard,
                            output_root,
                            local_workspace,
                            options.directory_sync_policy,
                            cancel_event,
                        )] = shard
            except BaseException:
                cancel_event.set()
                for future in pending:
                    future.cancel()
                raise
    verify_source_freeze(plan)
    results = _verified_results(output_root, plan, plan_sha256)
    report_path = output_root / "campaign-report.json"
    if report_path.exists():
        if report_path.is_symlink() or not report_path.is_file():
            raise ValueError("campaign report is not a nonsymlink file")
        report = validate_campaign_report(
            read_canonical_json(report_path), plan, plan_sha256, results
        )
        require_plan_snapshot(plan_path, plan_bytes)
        return report
    elapsed = time.perf_counter() - started
    report_elapsed = round(elapsed, 6)
    published_names = set(shard_by_id) - preexisting_names
    published_contracts = sum(
        int(shard_by_id[name]["contract_count"])
        for name in published_names
    )
    _, preexisting_digest = _digest_tuples(
        (name,) for name in sorted(preexisting_names)
    )
    _, published_digest = _digest_tuples(
        (name,) for name in sorted(published_names)
    )
    total_bytes = sum(int(result["artifact_bytes"]) for result in results)
    total_files = sum(int(result["artifact_file_count"]) + 2 for result in results)
    result_binding_count, result_binding_digest = _digest_tuples(
        (
            str(shard["shard_id"]),
            str(result["probe_report_sha256"]),
            str(result["probe_result_sha256"]),
            str(result["input_results_sha256"]),
            str(result["artifact_manifest_sha256"]),
        )
        for shard, result in zip(shards, results, strict=True)
    )
    campaign_sync = _fsync_directory(
        output_root, options.directory_sync_policy
    )
    shard_syncs = {
        str(observation)
        for result in results
        for observation in result["directory_sync_outcomes"].values()  # type: ignore[union-attr]
    }
    report = {
        "schema": REPORT_SCHEMA,
        "campaign_id": plan["campaign_id"],
        "campaign_plan_sha256": plan_sha256,
        "shards": len(shards),
        "contracts": plan["total_contracts"],
        "workers": options.workers,
        "invocation_elapsed_seconds": report_elapsed,
        "published_this_run_contracts_per_second": (
            round(published_contracts / report_elapsed, 6)
            if published_contracts
            else 0
        ),
        "preexisting_shard_count": len(preexisting_names),
        "preexisting_contract_count": preexisting_contracts,
        "preexisting_shard_ids": sorted(preexisting_names),
        "preexisting_shard_ids_sha256": preexisting_digest,
        "published_this_run_shard_count": len(published_names),
        "published_this_run_contract_count": published_contracts,
        "published_this_run_shard_ids": sorted(published_names),
        "published_this_run_shard_ids_sha256": published_digest,
        "sum_shard_peak_rss_kib": sum(int(result["peak_rss_kib"]) for result in results),
        "maximum_shard_peak_rss_kib": max(int(result["peak_rss_kib"]) for result in results),
        "published_artifact_bytes": total_bytes,
        "published_file_count": total_files,
        "directory_sync_policy": options.directory_sync_policy,
        "directory_sync_observations": sorted(shard_syncs | {campaign_sync}),
        "campaign_directory_sync_outcome": campaign_sync,
        "failures": 0,
        "semantic_promotion": "requires-exact-frozen-join",
        "published_probe_result_binding_count": result_binding_count,
        "published_probe_result_bindings_sha256": result_binding_digest,
    }
    payload = (canonical_json(report) + "\n").encode("utf-8")
    report_sync = atomic_write(
        report_path, payload, options.directory_sync_policy
    )
    if report_sync != campaign_sync:
        raise ValueError("campaign-root directory sync outcome changed")
    validated = validate_campaign_report(report, plan, plan_sha256, results)
    require_plan_snapshot(plan_path, plan_bytes)
    return validated


def verify_campaign_locked(
    plan_path: Path,
    plan: Mapping[str, object],
    output_root: Path,
    plan_bytes: bytes | None = None,
    local_temp_root: Path | None = None,
    minimum_local_free_gib: float = 0.0,
    verify_authority: bool = True,
    maximum_authority_database_bytes: int = (
        DEFAULT_MAX_AUTHORITY_DATABASE_BYTES
    ),
    maximum_authority_merge_scratch_bytes: int = (
        DEFAULT_MAX_AUTHORITY_MERGE_SCRATCH_BYTES
    ),
) -> Mapping[str, object]:
    if plan_bytes is None:
        plan_bytes = plan_path.read_bytes()
    require_plan_snapshot(plan_path, plan_bytes)
    allowed = {
        "campaign-plan.json", "campaign-report.json", "campaign.lock",
        "shards", ".staging",
    }
    if any(path.name not in allowed for path in output_root.iterdir()):
        raise ValueError("campaign output root contains unrelated entries")
    frozen = output_root / "campaign-plan.json"
    report_path = output_root / "campaign-report.json"
    for path in (frozen, report_path):
        if path.is_symlink() or not path.is_file():
            raise ValueError("campaign metadata is not a nonsymlink file")
    if frozen.read_bytes() != plan_bytes:
        raise ValueError("published campaign plan differs")
    plan_sha256 = hashlib.sha256(plan_bytes).hexdigest()
    verify_source_freeze(plan)
    if verify_authority:
        verify_authoritative_plan(
            plan,
            local_temp_root=local_temp_root,
            minimum_local_free_gib=minimum_local_free_gib,
            maximum_database_bytes=maximum_authority_database_bytes,
            maximum_merge_scratch_bytes=(
                maximum_authority_merge_scratch_bytes
            ),
        )
    results = _verified_results(output_root, plan, plan_sha256)
    report = validate_campaign_report(
        read_canonical_json(report_path), plan, plan_sha256, results
    )
    verify_source_freeze(plan)
    require_plan_snapshot(plan_path, plan_bytes)
    return report


def verify_campaign(options: argparse.Namespace) -> Mapping[str, object]:
    plan_path = require_regular_file(options.plan, "campaign plan")
    output_root = require_root(options.output_root, "output root")
    local_temp_root = require_root(
        options.local_temp_root, "verification local temp root"
    )
    require_free_space(
        local_temp_root,
        options.minimum_local_free_gib,
        "verification local",
    )
    preflight_campaign_output_root(output_root)
    with campaign_lock(output_root, "record-unsupported", create=False):
        preflight_campaign_output_root(output_root)
        plan_bytes = plan_path.read_bytes()
        plan = validate_plan(parse_canonical_json(plan_bytes, str(plan_path)))
        validate_run_root_isolation(
            plan_path, plan, output_root, local_temp_root
        )
        return verify_campaign_locked(
            plan_path,
            plan,
            output_root,
            plan_bytes,
            local_temp_root,
            options.minimum_local_free_gib,
            True,
            options.maximum_authority_database_bytes,
            options.maximum_authority_merge_scratch_bytes,
        )


def main() -> int:
    # Avoid loading a second copy when this executable's shard subcommand
    # imports the shared campaign primitives.
    sys.modules.setdefault("run_a64_aslref_campaign", sys.modules[__name__])
    import a64_aslref_campaign_sharding as sharding

    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="action", required=True)
    shard_parser = subparsers.add_parser("shard")
    sharding.add_arguments(shard_parser)
    plan_parser = subparsers.add_parser("plan")
    plan_arguments(plan_parser)
    run_parser = subparsers.add_parser("run")
    run_arguments(run_parser)
    pilot_parser = subparsers.add_parser("pilot")
    run_arguments(pilot_parser)
    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("--plan", type=Path, required=True)
    verify_parser.add_argument("--output-root", type=Path, required=True)
    verify_parser.add_argument("--local-temp-root", type=Path, required=True)
    verify_parser.add_argument(
        "--minimum-local-free-gib", type=float, default=20.0
    )
    verify_parser.add_argument(
        "--maximum-authority-database-bytes",
        type=int,
        default=DEFAULT_MAX_AUTHORITY_DATABASE_BYTES,
    )
    verify_parser.add_argument(
        "--maximum-authority-merge-scratch-bytes",
        type=int,
        default=DEFAULT_MAX_AUTHORITY_MERGE_SCRATCH_BYTES,
    )
    options = parser.parse_args()
    if options.action == "shard":
        result = sharding.run(options)
    elif options.action == "plan":
        result = build_plan(options)
    elif options.action == "run":
        result = run_campaign(options)
    elif options.action == "pilot":
        result = pilot_campaign(options)
    else:
        result = verify_campaign(options)
    print(canonical_json(result))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        OSError,
        ValueError,
        sqlite3.DatabaseError,
    ) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
