#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import struct
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import BinaryIO


BOOT_MAGIC = b"ANDROID!"
BOOT_HEADER_VERSION = 4
BOOT_HEADER_SIZE = 1584
BOOT_PAGE_SIZE = 4096
COPY_CHUNK = 1024 * 1024


class BootImageError(ValueError):
    pass


@dataclass(frozen=True)
class BootImage:
    container_offset: int
    container_size: int
    header_version: int
    header_size: int
    kernel_size: int
    ramdisk_offset: int
    ramdisk_size: int
    signature_size: int


def read_exact(source: BinaryIO, offset: int, size: int) -> bytes:
    source.seek(offset)
    payload = source.read(size)
    if len(payload) != size:
        raise BootImageError("boot image is truncated")
    return payload


def parse(source: BinaryIO, container_offset: int, container_size: int) -> BootImage:
    if container_offset < 0 or container_size < BOOT_PAGE_SIZE:
        raise BootImageError("boot image range is invalid")
    header = read_exact(source, container_offset, BOOT_PAGE_SIZE)
    if header[:8] != BOOT_MAGIC:
        raise BootImageError("Android boot magic is missing")
    kernel_size, ramdisk_size, _os_version, header_size = struct.unpack_from(
        "<IIII", header, 8
    )
    reserved = struct.unpack_from("<4I", header, 24)
    header_version = struct.unpack_from("<I", header, 40)[0]
    signature_size = struct.unpack_from("<I", header, 1580)[0]
    if header_version != BOOT_HEADER_VERSION or header_size != BOOT_HEADER_SIZE:
        raise BootImageError("boot image must use the exact version-4 header")
    if any(reserved):
        raise BootImageError("boot image reserved header words are not zero")
    if any(header[BOOT_HEADER_SIZE:BOOT_PAGE_SIZE]):
        raise BootImageError("boot image header padding is not zero")
    if kernel_size != 0:
        raise BootImageError("boot image must not embed the independently signed kernel")
    if ramdisk_size <= 0:
        raise BootImageError("boot image ramdisk is empty")
    if signature_size != 0:
        raise BootImageError("boot image must not carry a separate boot signature")

    ramdisk_offset = container_offset + BOOT_PAGE_SIZE
    if ramdisk_offset + ramdisk_size > container_offset + container_size:
        raise BootImageError("boot image ramdisk exceeds its container")
    return BootImage(
        container_offset,
        container_size,
        header_version,
        header_size,
        kernel_size,
        ramdisk_offset,
        ramdisk_size,
        signature_size,
    )


def compare_ramdisk(source: BinaryIO, image: BootImage, expected_path: Path) -> None:
    if expected_path.is_symlink() or not expected_path.is_file():
        raise BootImageError(f"ramdisk must be a regular non-symlink file: {expected_path}")
    if expected_path.stat().st_size != image.ramdisk_size:
        raise BootImageError("embedded ramdisk size differs from the expected input")
    source.seek(image.ramdisk_offset)
    with expected_path.open("rb") as expected:
        remaining = image.ramdisk_size
        while remaining:
            size = min(remaining, COPY_CHUNK)
            actual_block = source.read(size)
            expected_block = expected.read(size)
            if actual_block != expected_block or len(actual_block) != size:
                raise BootImageError("embedded ramdisk differs from the expected input")
            remaining -= size
        if expected.read(1):
            raise BootImageError("expected ramdisk grew while it was compared")


def regular_file(path: Path, label: str) -> Path:
    if path.is_symlink() or not path.is_file():
        raise BootImageError(f"{label} must be a regular non-symlink file: {path}")
    return path.resolve()


def partition_range(layout_path: Path, disk_path: Path, name: str) -> tuple[int, int]:
    try:
        document = json.loads(layout_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise BootImageError(f"cannot read partition layout: {error}") from error
    if not isinstance(document, dict) or document.get("schema_version") != 1:
        raise BootImageError("partition layout schema is invalid")
    disk_size = document.get("disk_size_bytes")
    partitions = document.get("partitions")
    if isinstance(disk_size, bool) or not isinstance(disk_size, int):
        raise BootImageError("partition layout disk size is invalid")
    if not isinstance(partitions, list):
        raise BootImageError("partition layout inventory is invalid")
    matches = [
        entry
        for entry in partitions
        if isinstance(entry, dict) and entry.get("name") == name
    ]
    if len(matches) != 1:
        raise BootImageError(f"partition is absent or repeated: {name}")
    start = matches[0].get("start_bytes")
    size = matches[0].get("size_bytes")
    if (
        isinstance(start, bool)
        or not isinstance(start, int)
        or isinstance(size, bool)
        or not isinstance(size, int)
        or start < 0
        or size <= 0
        or start + size > disk_size
    ):
        raise BootImageError(f"partition range is invalid: {name}")
    if disk_path.stat().st_size != disk_size:
        raise BootImageError("disk size differs from the checked partition layout")
    return start, size


def inspect(args: argparse.Namespace) -> tuple[Path, BootImage]:
    if args.image is not None:
        if args.disk is not None or args.layout is not None or args.partition is not None:
            raise BootImageError("--image cannot be combined with disk partition arguments")
        path = regular_file(args.image, "boot image")
        offset, size = 0, path.stat().st_size
    else:
        if args.disk is None or args.layout is None or args.partition is None:
            raise BootImageError("disk inspection requires --disk, --layout, and --partition")
        path = regular_file(args.disk, "disk image")
        layout_path = regular_file(args.layout, "partition layout")
        offset, size = partition_range(layout_path, path, args.partition)
    with path.open("rb") as source:
        image = parse(source, offset, size)
        if args.ramdisk is not None:
            compare_ramdisk(source, image, args.ramdisk)
    return path, image


def add_source_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--image", type=Path)
    parser.add_argument("--disk", type=Path)
    parser.add_argument("--layout", type=Path)
    parser.add_argument("--partition")
    parser.add_argument("--ramdisk", type=Path)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Inspect kernelless Android boot-image v4 containers"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    verify_parser = subparsers.add_parser("verify")
    add_source_arguments(verify_parser)
    range_parser = subparsers.add_parser("range")
    add_source_arguments(range_parser)
    args = parser.parse_args()
    try:
        path, image = inspect(args)
        if args.command == "range":
            print(json.dumps(asdict(image), sort_keys=True, separators=(",", ":")))
        else:
            print(
                f"verified kernelless Android boot image v4: {path}, "
                f"ramdisk={image.ramdisk_offset}+{image.ramdisk_size}"
            )
    except (BootImageError, OSError, struct.error) as error:
        raise SystemExit(f"Android boot image: {error}") from error


if __name__ == "__main__":
    main()
