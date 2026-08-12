#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Losslessly replace a complete raw-evidence profile with gzip."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import gzip
import math
import os
import shutil
import tempfile
from pathlib import Path

from a64_raw_evidence_files import (
    COMPRESSED_SUFFIX,
    PLAIN_SUFFIX,
    RawEvidenceFile,
    canonical_existing_directory,
    producer_set_sha256,
    require_regular_raw_file,
    validate_raw_profile,
    validate_producer_authority,
)


SCHEMA = "uml.a64.raw-evidence-compression/v2"


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-directory", type=Path, required=True)
    parser.add_argument(
        "--guest-page-size", type=int, choices=(4096, 16384), required=True
    )
    parser.add_argument("--free-space-reserve-gib", type=float, default=8.0)
    return parser.parse_args()


def verify_gzip(path: Path) -> None:
    require_regular_raw_file(path)
    with path.open("rb") as source:
        header = source.read(3)
    if header != b"\x1f\x8b\x08":
        raise ValueError(f"raw evidence is not gzip data: {path}")


def compress_plain_raw(
    producer: str, plain: Path, compressed: Path, page: str
) -> RawEvidenceFile:
    require_regular_raw_file(plain)
    original = RawEvidenceFile(producer, plain, False)
    validate_raw_profile(original, page)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{compressed.name}.", suffix=".tmp", dir=compressed.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as destination:
            with gzip.GzipFile(mode="wb", fileobj=destination) as encoded:
                with plain.open("rb") as source:
                    shutil.copyfileobj(source, encoded, length=1024 * 1024)
        temporary_entry = RawEvidenceFile(producer, temporary, True)
        verify_gzip(temporary)
        validate_raw_profile(temporary_entry, page)
        os.replace(temporary, compressed)
        plain.unlink()
        return RawEvidenceFile(producer, compressed, True)
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
        verify_gzip(compressed)
    if plain is not None and compressed is not None:
        # A prior interruption may leave both names. Recompress the semantic
        # source through a fresh temporary file and replace the old gzip.
        return compress_plain_raw(producer, plain, compressed, page)
    if compressed is not None:
        entry = RawEvidenceFile(producer, compressed, True)
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


def run(options: argparse.Namespace) -> dict[str, object]:
    options.evidence_directory = canonical_existing_directory(
        options.evidence_directory, "raw evidence directory"
    )
    discovered = discover_for_compression(options.evidence_directory)
    producers = set(discovered)
    page = str(options.guest_page_size)
    validate_producer_authority(producers, page)
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
        records.append(
            {
                "producer": producer,
                "file": entry.path.name,
                "row_count": row_count,
                "compressed_bytes": entry.path.stat().st_size,
            }
        )
    summary: dict[str, object] = {
        "schema": SCHEMA,
        "guest_page_size": options.guest_page_size,
        "producer_count": len(producers),
        "producer_set_sha256": producer_set_sha256(producers),
        "records": records,
    }
    return summary


def main() -> int:
    options = arguments()
    result = run(options)
    print(
        f"A64 raw evidence compressed: {result['producer_count']} producers, "
        f"page={result['guest_page_size']}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=__import__("sys").stderr)
        raise SystemExit(1)
