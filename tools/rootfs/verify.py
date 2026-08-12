#!/usr/bin/env python3
"""Authenticate a rootfs artifact before provisioning it."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import struct
import sys
import uuid
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = ROOT / "tools" / "rootfs" / "manifest.json"
EROFS_SUPERBLOCK_OFFSET = 1024
EROFS_SUPERBLOCK_SIZE = 144
EROFS_MAGIC = 0xE0F5E1E2


class VerificationError(RuntimeError):
    """The image does not match the checked-in EROFS fixture contract."""


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


def hexadecimal(value: Any, field: str) -> int:
    try:
        return int(text(value, field), 16)
    except ValueError as error:
        raise VerificationError(f"manifest field {field} must be hexadecimal") from error


def parse_erofs_superblock(path: Path, manifest: dict[str, Any]) -> dict[str, Any]:
    layout = manifest.get("filesystem")
    if not isinstance(layout, dict) or layout.get("format") != "erofs":
        raise VerificationError("manifest has no EROFS filesystem contract")
    offset = integer(layout.get("superblock_offset"), "filesystem.superblock_offset")
    if offset != EROFS_SUPERBLOCK_OFFSET:
        raise VerificationError("unsupported EROFS superblock offset")

    with path.open("rb") as image:
        image.seek(offset)
        superblock = image.read(EROFS_SUPERBLOCK_SIZE)
    if len(superblock) != EROFS_SUPERBLOCK_SIZE:
        raise VerificationError("rootfs EROFS superblock is truncated")

    magic = struct.unpack_from("<I", superblock, 0)[0]
    if magic != EROFS_MAGIC:
        raise VerificationError("rootfs does not have an EROFS superblock")
    blkszbits = superblock[12]
    if blkszbits < 9 or blkszbits > 16:
        raise VerificationError("rootfs EROFS block-size shift is invalid")
    block_size = 1 << blkszbits
    blocks = struct.unpack_from("<I", superblock, 36)[0]
    if blocks == 0 or blocks * block_size != path.stat().st_size:
        raise VerificationError("rootfs EROFS block count does not cover the image")
    try:
        volume_name = superblock[64:80].split(b"\0", 1)[0].decode("utf-8")
    except UnicodeDecodeError as error:
        raise VerificationError("rootfs EROFS volume name is not UTF-8") from error

    actual = {
        "magic": magic,
        "checksum": struct.unpack_from("<I", superblock, 4)[0],
        "feature_compat": struct.unpack_from("<I", superblock, 8)[0],
        "block_size": block_size,
        "blocks": blocks,
        "inodes": struct.unpack_from("<Q", superblock, 16)[0],
        "uuid": str(uuid.UUID(bytes=superblock[48:64])),
        "volume_name": volume_name,
        "feature_incompat": struct.unpack_from("<I", superblock, 80)[0],
        "available_compr_algs": struct.unpack_from("<H", superblock, 84)[0],
        "extra_devices": struct.unpack_from("<H", superblock, 86)[0],
    }
    expected = {
        "magic": hexadecimal(layout.get("magic"), "filesystem.magic"),
        "checksum": hexadecimal(layout.get("checksum"), "filesystem.checksum"),
        "feature_compat": hexadecimal(
            layout.get("feature_compat"), "filesystem.feature_compat"
        ),
        "block_size": integer(layout.get("block_size"), "filesystem.block_size"),
        "blocks": integer(layout.get("blocks"), "filesystem.blocks"),
        "inodes": integer(layout.get("inodes"), "filesystem.inodes"),
        "uuid": text(layout.get("uuid"), "filesystem.uuid"),
        "volume_name": text(layout.get("volume_name"), "filesystem.volume_name"),
        "feature_incompat": hexadecimal(
            layout.get("feature_incompat"), "filesystem.feature_incompat"
        ),
        "available_compr_algs": hexadecimal(
            layout.get("available_compr_algs"),
            "filesystem.available_compr_algs",
        ),
        "extra_devices": integer(
            layout.get("extra_devices"), "filesystem.extra_devices"
        ),
    }
    for field, expected_value in expected.items():
        if actual[field] != expected_value:
            raise VerificationError(
                f"rootfs EROFS {field} does not match manifest"
            )
    return actual


def verify(image: Path, manifest_path: Path) -> dict[str, Any]:
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise VerificationError(f"cannot read rootfs manifest {manifest_path}: {error}") from error
    if not isinstance(manifest, dict):
        raise VerificationError("rootfs manifest must be a JSON object")
    if manifest.get("schema_version") != 3:
        raise VerificationError("rootfs manifest has an unsupported schema")
    if text(manifest.get("image"), "image") != "rootfs.erofs":
        raise VerificationError("rootfs manifest does not name rootfs.erofs")
    if not image.is_file():
        raise VerificationError(f"rootfs is not a regular file: {image}")
    details = image.stat()
    expected_size = integer(manifest.get("size"), "size")
    if details.st_size != expected_size:
        raise VerificationError(f"rootfs size is {details.st_size}, expected {expected_size}")
    actual_digest = digest(image)
    expected_digest = text(manifest.get("sha256"), "sha256")
    if actual_digest != expected_digest:
        raise VerificationError("rootfs SHA-256 does not match manifest")
    filesystem = parse_erofs_superblock(image, manifest)
    return {
        "image": str(image),
        "sha256": actual_digest,
        "size": details.st_size,
        "filesystem": "erofs",
        "uuid": filesystem["uuid"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=ROOT / "rootfs.erofs")
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
