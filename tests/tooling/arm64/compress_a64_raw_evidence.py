#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Losslessly replace a complete raw-evidence profile with deterministic gzip."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import gzip
import hashlib
import json
import math
import os
import shutil
import stat
import tempfile
from pathlib import Path

from a64_raw_evidence_files import (
    COMPRESSED_SUFFIX,
    PLAIN_SUFFIX,
    RawEvidenceFile,
    evidence_quiescence_lock_path,
    exclusive_evidence_locks,
    path_is_ancestor,
    paths_alias,
    publication_target_locks,
    producer_set_sha256,
    require_no_symlink_components,
    require_regular_raw_file,
    uncompressed_identity,
    validate_raw_profile,
    validate_producer_authority,
)


SCHEMA = "uml.a64.raw-evidence-compression/v1"


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-directory", type=Path, required=True)
    parser.add_argument(
        "--guest-page-size", type=int, choices=(4096, 16384), required=True
    )
    parser.add_argument("--identity-output", type=Path)
    parser.add_argument("--free-space-reserve-gib", type=float, default=8.0)
    return parser.parse_args()


def canonical_json(value: object) -> str:
    return json.dumps(
        value, ensure_ascii=True, sort_keys=True, separators=(",", ":")
    )


def file_identity(path: Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            size += len(chunk)
            digest.update(chunk)
    return size, digest.hexdigest()


def verify_deterministic_gzip(path: Path) -> None:
    require_regular_raw_file(path)
    with path.open("rb") as source:
        header = source.read(10)
    if (
        len(header) != 10
        or header[:3] != b"\x1f\x8b\x08"
        or header[3] != 0
        or header[4:8] != b"\0\0\0\0"
    ):
        raise ValueError(f"raw evidence gzip header is not deterministic: {path}")


def fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def fsync_file(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def require_uncompressed_identity(
    entry: RawEvidenceFile,
    expected: tuple[int, str],
    error: str,
) -> None:
    if uncompressed_identity(entry) != expected:
        raise ValueError(error)


def compress_plain_raw(
    producer: str, plain: Path, compressed: Path, page: str
) -> RawEvidenceFile:
    require_regular_raw_file(plain)
    if compressed.exists() or compressed.is_symlink():
        raise ValueError(
            f"refusing to overwrite existing compressed raw evidence: {compressed}"
        )
    original = RawEvidenceFile(producer, plain, False)
    validate_raw_profile(original, page)
    original_identity = uncompressed_identity(original)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{compressed.name}.", suffix=".tmp", dir=compressed.parent
    )
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, stat.S_IMODE(plain.stat().st_mode))
        with os.fdopen(descriptor, "wb") as destination:
            with gzip.GzipFile(
                filename="", mode="wb", fileobj=destination, mtime=0
            ) as encoded:
                with plain.open("rb") as source:
                    shutil.copyfileobj(source, encoded, length=1024 * 1024)
            destination.flush()
            os.fsync(destination.fileno())
        temporary_entry = RawEvidenceFile(producer, temporary, True)
        verify_deterministic_gzip(temporary)
        if (
            uncompressed_identity(temporary_entry) != original_identity
            or validate_raw_profile(temporary_entry, page) == 0
        ):
            raise ValueError(f"compressed raw evidence identity differs: {plain}")
        if compressed.exists() or compressed.is_symlink():
            raise ValueError(
                f"refusing to overwrite existing compressed raw evidence: "
                f"{compressed}"
            )
        os.replace(temporary, compressed)
        fsync_directory(compressed.parent)
        final = RawEvidenceFile(producer, compressed, True)
        verify_deterministic_gzip(compressed)
        if (
            uncompressed_identity(final) != original_identity
            or validate_raw_profile(final, page) == 0
        ):
            raise ValueError(f"published raw evidence identity differs: {plain}")
        fsync_file(compressed)
        require_uncompressed_identity(
            original,
            original_identity,
            f"plain raw evidence changed before deletion: {plain}",
        )
        plain.unlink()
        fsync_directory(compressed.parent)
        return final
    finally:
        if temporary.exists():
            temporary.unlink()


def resolve_entry(
    producer: str,
    plain: Path | None,
    compressed: Path | None,
    page: str,
) -> RawEvidenceFile:
    if plain is not None:
        require_regular_raw_file(plain)
    if compressed is not None:
        verify_deterministic_gzip(compressed)
    if plain is not None and compressed is not None:
        plain_entry = RawEvidenceFile(producer, plain, False)
        compressed_entry = RawEvidenceFile(producer, compressed, True)
        validate_raw_profile(plain_entry, page)
        validate_raw_profile(compressed_entry, page)
        plain_identity = uncompressed_identity(plain_entry)
        if plain_identity != uncompressed_identity(compressed_entry):
            raise ValueError(
                f"plain/compressed recovery identity differs: {producer}"
            )
        fsync_file(compressed)
        require_uncompressed_identity(
            plain_entry,
            plain_identity,
            f"plain raw evidence changed before recovery deletion: {producer}",
        )
        plain.unlink()
        fsync_directory(plain.parent)
        return compressed_entry
    if compressed is not None:
        # Reading the whole stream proves its trailer before the plain source
        # can be considered durably replaced.
        entry = RawEvidenceFile(producer, compressed, True)
        uncompressed_identity(entry)
        validate_raw_profile(entry, page)
        return entry
    if plain is None:
        raise ValueError(f"raw evidence producer has no file: {producer}")
    return compress_plain_raw(
        producer, plain, plain.with_name(plain.name + ".gz"), page
    )


def discover_for_compression(
    directory: Path,
) -> dict[str, tuple[Path | None, Path | None]]:
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError(f"raw evidence path is not a regular directory: {directory}")
    result: dict[str, list[Path | None]] = {}
    for suffix, index in ((PLAIN_SUFFIX, 0), (COMPRESSED_SUFFIX, 1)):
        for path in sorted(directory.glob(f"*{suffix}")):
            require_regular_raw_file(path)
            producer = path.name[: -len(suffix)]
            slots = result.setdefault(producer, [None, None])
            slots[index] = path
    return {
        producer: (paths[0], paths[1])
        for producer, paths in sorted(result.items())
    }


def atomic_write(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        mode = stat.S_IMODE(path.stat().st_mode) if path.exists() else 0o644
        os.fchmod(descriptor, mode)
        with os.fdopen(descriptor, "wb") as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        fsync_directory(path.parent)
    finally:
        if temporary.exists():
            temporary.unlink()


def write_or_compare(path: Path, payload: bytes) -> None:
    if path.exists() or path.is_symlink():
        require_regular_raw_file(path)
        if path.read_bytes() != payload:
            raise ValueError(
                f"raw compression identity already exists with different content: "
                f"{path}"
            )
        fsync_file(path)
        fsync_directory(path.parent)
        return
    atomic_write(path, payload)


def reserve_bytes(value: float) -> int:
    if not math.isfinite(value) or value < 0:
        raise ValueError("raw compression reserve must be finite and nonnegative")
    return int(value * 1024 * 1024 * 1024)


def gzip_temporary_worst_case_bytes(raw_size: int) -> int:
    if raw_size < 0:
        raise ValueError("raw evidence size must be nonnegative")
    # DEFLATE codes are at most 15 bits per input symbol.  Twice the input
    # plus 1 MiB for block, wrapper, and buffering overhead is therefore a
    # deliberately conservative hard upper bound for this gzip member.
    return raw_size * 2 + 1024 * 1024


def compression_lock_path(directory: Path) -> Path:
    return evidence_quiescence_lock_path(directory)


def compression_lock(directory: Path):
    return exclusive_evidence_locks((directory,))


def validate_identity_output(
    discovered: dict[str, tuple[Path | None, Path | None]],
    output: Path,
    lock_path: Path | None = None,
) -> None:
    """Reject aliases before any plain evidence can be replaced."""
    candidates = [
        candidate
        for plain, compressed in discovered.values()
        for candidate in (
            plain,
            compressed,
            None if plain is None else plain.with_name(plain.name + ".gz"),
        )
        if candidate is not None
    ]
    output = require_no_symlink_components(
        output, "raw compression identity output"
    )
    checked_candidates = tuple(
        require_no_symlink_components(candidate, "raw evidence path")
        for candidate in candidates
    )
    if lock_path is not None:
        lock_path = require_no_symlink_components(
            lock_path, "raw evidence quiescence lock"
        )
        if paths_alias(output, lock_path):
            raise ValueError("raw compression identity output aliases lock file")
        if path_is_ancestor(output, lock_path) or path_is_ancestor(
            lock_path, output
        ):
            raise ValueError(
                "raw compression identity output has impossible lock ancestry"
            )
    if any(paths_alias(candidate, output) for candidate in checked_candidates):
        raise ValueError("raw compression identity output aliases raw evidence")
    if any(
        path_is_ancestor(candidate, output)
        or path_is_ancestor(output, candidate)
        for candidate in checked_candidates
    ):
        raise ValueError(
            "raw compression identity output has impossible raw-file ancestry"
        )
    if output.is_symlink() or (output.exists() and not output.is_file()):
        raise ValueError(
            "raw compression identity output is not a nonsymlink regular file"
        )


def recheck_compressed_records(
    directory: Path,
    page: str,
    expected_records: list[dict[str, object]],
) -> None:
    """Re-discover and exact-compare every final stream before identity write."""
    discovered = discover_for_compression(directory)
    validate_producer_authority(set(discovered), page)
    expected_by_producer = {
        str(record["producer"]): record for record in expected_records
    }
    if len(expected_by_producer) != len(expected_records) or set(
        expected_by_producer
    ) != set(discovered):
        raise ValueError("raw compression final producer set differs")
    for producer, (plain, compressed) in discovered.items():
        if plain is not None or compressed is None:
            raise ValueError(
                f"raw compression final producer is not uniquely compressed: "
                f"{producer}"
            )
        entry = RawEvidenceFile(producer, compressed, True)
        verify_deterministic_gzip(compressed)
        row_count = validate_raw_profile(entry, page)
        raw_size, raw_sha256 = uncompressed_identity(entry)
        compressed_size, compressed_sha256 = file_identity(compressed)
        observed: dict[str, object] = {
            "producer": producer,
            "file": compressed.name,
            "raw_size": raw_size,
            "raw_sha256": raw_sha256,
            "row_count": row_count,
            "compressed_size": compressed_size,
            "compressed_sha256": compressed_sha256,
        }
        if observed != expected_by_producer[producer]:
            raise ValueError(
                f"raw compression final identity differs: {producer}"
            )
def _run_locked(options: argparse.Namespace) -> dict[str, object]:
    discovered = discover_for_compression(options.evidence_directory)
    producers = set(discovered)
    page = str(options.guest_page_size)
    validate_producer_authority(producers, page)
    output = options.identity_output
    if output is None:
        output = (
            options.evidence_directory
            / "a64-raw-evidence-compression.identity.json"
        )
    validate_identity_output(
        discovered, output, compression_lock_path(options.evidence_directory)
    )
    reserve = reserve_bytes(options.free_space_reserve_gib)
    records: list[dict[str, object]] = []
    for producer, (plain, compressed) in discovered.items():
        if plain is not None and compressed is None:
            required = reserve + gzip_temporary_worst_case_bytes(
                plain.stat().st_size
            )
            free = shutil.disk_usage(options.evidence_directory).free
            if free < required:
                raise ValueError(
                    f"insufficient free space for raw compression: "
                    f"free={free}, required={required}, producer={producer}"
                )
        entry = resolve_entry(producer, plain, compressed, page)
        row_count = validate_raw_profile(entry, page)
        raw_size, raw_sha256 = uncompressed_identity(entry)
        compressed_size, compressed_sha256 = file_identity(entry.path)
        records.append(
            {
                "producer": producer,
                "file": entry.path.name,
                "raw_size": raw_size,
                "raw_sha256": raw_sha256,
                "row_count": row_count,
                "compressed_size": compressed_size,
                "compressed_sha256": compressed_sha256,
            }
        )
    identity: dict[str, object] = {
        "schema": SCHEMA,
        "guest_page_size": options.guest_page_size,
        "producer_count": len(producers),
        "producer_set_sha256": producer_set_sha256(producers),
        "records": records,
    }
    payload = (canonical_json(identity) + "\n").encode("utf-8")
    recheck_compressed_records(
        options.evidence_directory, page, records
    )
    write_or_compare(output, payload)
    return identity


def run(options: argparse.Namespace) -> dict[str, object]:
    with compression_lock(options.evidence_directory) as directories:
        options.evidence_directory = directories[0]
        discovered = discover_for_compression(options.evidence_directory)
        output = options.identity_output
        if output is None:
            output = (
                options.evidence_directory
                / "a64-raw-evidence-compression.identity.json"
            )
        validate_identity_output(
            discovered,
            output,
            compression_lock_path(options.evidence_directory),
        )
        with publication_target_locks((output,)) as outputs:
            options.identity_output = outputs[0]
            return _run_locked(options)


def main() -> int:
    options = arguments()
    identity = run(options)
    print(
        f"A64 raw evidence compressed: {identity['producer_count']} producers, "
        f"page={identity['guest_page_size']}, identity="
        f"{identity['producer_set_sha256']}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=__import__("sys").stderr)
        raise SystemExit(1)
