#!/usr/bin/env python3
"""Verify the immutable rootfs image before and after guest campaigns."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import stat
import struct
import sys
import uuid
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = ROOT / "tools" / "rootfs" / "manifest.json"


class VerificationError(RuntimeError):
    """The image does not match the checked-in immutable fixture contract."""


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as image:
        while chunk := image.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def integer(value: Any, field: str) -> int:
    if not isinstance(value, int):
        raise VerificationError(f"manifest field {field} must be an integer")
    return value


def text(value: Any, field: str) -> str:
    if not isinstance(value, str):
        raise VerificationError(f"manifest field {field} must be a string")
    return value


def parse_partition_table(path: Path, manifest: dict[str, Any]) -> None:
    layout = manifest.get("partition_table")
    if not isinstance(layout, dict):
        raise VerificationError("manifest has no partition_table")
    sector_size = integer(layout.get("sector_size"), "partition_table.sector_size")
    if sector_size != 512:
        raise VerificationError("only a 512-byte-sector rootfs fixture is supported")
    mbr_expected = layout.get("protective_mbr")
    gpt_expected = layout.get("gpt")
    partitions = layout.get("partitions")
    if not isinstance(mbr_expected, dict) or not isinstance(gpt_expected, dict):
        raise VerificationError("manifest partition-table metadata is incomplete")
    if not isinstance(partitions, list) or not partitions:
        raise VerificationError("manifest has no required partition entries")

    with path.open("rb") as image:
        mbr = image.read(sector_size)
        if len(mbr) != sector_size or mbr[510:512] != b"\x55\xaa":
            raise VerificationError("rootfs does not have a valid protective MBR")
        entry = mbr[446:462]
        if entry[4] != int(text(mbr_expected.get("type"), "protective_mbr.type"), 16):
            raise VerificationError("protective MBR type does not match manifest")
        first_lba, sector_count = struct.unpack_from("<II", entry, 8)
        if first_lba != integer(mbr_expected.get("first_lba"), "protective_mbr.first_lba"):
            raise VerificationError("protective MBR first LBA does not match manifest")
        if sector_count != integer(mbr_expected.get("sector_count"), "protective_mbr.sector_count"):
            raise VerificationError("protective MBR sector count does not match manifest")

        image.seek(sector_size * integer(gpt_expected.get("header_lba"), "gpt.header_lba"))
        header = image.read(sector_size)
        if header[:8] != b"EFI PART":
            raise VerificationError("rootfs does not have a GPT header")
        header_size = struct.unpack_from("<I", header, 12)[0]
        if header_size < 92 or header_size > sector_size:
            raise VerificationError("rootfs GPT header size is invalid")
        current_lba = struct.unpack_from("<Q", header, 24)[0]
        first_usable_lba, last_usable_lba = struct.unpack_from("<QQ", header, 40)
        entries_lba = struct.unpack_from("<Q", header, 72)[0]
        entry_count, entry_size = struct.unpack_from("<II", header, 80)
        expected_values = {
            "header_lba": current_lba,
            "first_usable_lba": first_usable_lba,
            "last_usable_lba": last_usable_lba,
            "partition_entry_lba": entries_lba,
            "partition_entry_count": entry_count,
            "partition_entry_size": entry_size,
        }
        for field, actual in expected_values.items():
            if actual != integer(gpt_expected.get(field), f"gpt.{field}"):
                raise VerificationError(f"GPT {field} does not match manifest")

        for expected in partitions:
            if not isinstance(expected, dict):
                raise VerificationError("manifest partition entry is not an object")
            number = integer(expected.get("number"), "partition.number")
            if number < 1 or number > entry_count:
                raise VerificationError(f"partition number {number} is outside the GPT table")
            image.seek(entries_lba * sector_size + (number - 1) * entry_size)
            entry = image.read(entry_size)
            if len(entry) != entry_size:
                raise VerificationError(f"partition {number} entry is truncated")
            type_guid = str(uuid.UUID(bytes_le=entry[:16]))
            first_lba, last_lba = struct.unpack_from("<QQ", entry, 32)
            if type_guid != text(expected.get("type_guid"), "partition.type_guid"):
                raise VerificationError(f"partition {number} type GUID does not match manifest")
            if first_lba != integer(expected.get("first_lba"), "partition.first_lba"):
                raise VerificationError(f"partition {number} first LBA does not match manifest")
            if last_lba != integer(expected.get("last_lba"), "partition.last_lba"):
                raise VerificationError(f"partition {number} last LBA does not match manifest")


def verify(image: Path, manifest_path: Path) -> dict[str, Any]:
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise VerificationError(f"cannot read rootfs manifest {manifest_path}: {error}") from error
    if not image.is_file():
        raise VerificationError(f"rootfs is not a regular file: {image}")
    details = image.stat()
    expected_size = integer(manifest.get("size"), "size")
    if details.st_size != expected_size:
        raise VerificationError(f"rootfs size is {details.st_size}, expected {expected_size}")
    expected_mode = int(text(manifest.get("frozen_mode"), "frozen_mode"), 8)
    actual_mode = stat.S_IMODE(details.st_mode)
    if actual_mode != expected_mode:
        raise VerificationError(f"rootfs mode is {actual_mode:04o}, expected {expected_mode:04o}")
    actual_digest = digest(image)
    expected_digest = text(manifest.get("sha256"), "sha256")
    if actual_digest != expected_digest:
        raise VerificationError("rootfs SHA-256 does not match manifest")
    parse_partition_table(image, manifest)
    return {"image": str(image), "sha256": actual_digest, "size": details.st_size, "mode": f"{actual_mode:04o}"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=ROOT / "rootfs.img")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--quiet", action="store_true")
    arguments = parser.parse_args()
    try:
        result = verify(arguments.image.resolve(), arguments.manifest.resolve())
    except VerificationError as error:
        print(f"rootfs verification failed: {error}", file=sys.stderr)
        return 1
    if not arguments.quiet:
        print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
