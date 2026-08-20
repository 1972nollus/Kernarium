#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import binascii
import json
import os
import re
import struct
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO


GPT_SIGNATURE = b"EFI PART"
GPT_REVISION = 0x00010000
GPT_HEADER_SIZE = 92
GPT_ENTRY_SIZE = 128
PROTECTIVE_MBR_TYPE = 0xEE
SPARSE_MAGIC = 0xED26FF3A
SPARSE_HEADER = struct.Struct("<IHHHHIIII")
SPARSE_CHUNK_HEADER = struct.Struct("<HHII")
SPARSE_RAW = 0xCAC1
SPARSE_FILL = 0xCAC2
SPARSE_DONT_CARE = 0xCAC3
SPARSE_CRC32 = 0xCAC4
COPY_CHUNK = 1024 * 1024


class DiskError(ValueError):
    pass


@dataclass(frozen=True)
class Partition:
    name: str
    start_bytes: int
    size_bytes: int
    image: str
    type_guid: uuid.UUID
    unique_guid: uuid.UUID
    writable: bool


@dataclass(frozen=True)
class Layout:
    sector_size: int
    alignment_bytes: int
    disk_size_bytes: int
    partition_entry_count: int
    disk_guid: uuid.UUID
    partitions: tuple[Partition, ...]

    @property
    def sector_count(self) -> int:
        return self.disk_size_bytes // self.sector_size

    @property
    def table_bytes(self) -> int:
        return self.partition_entry_count * GPT_ENTRY_SIZE

    @property
    def table_sectors(self) -> int:
        return self.table_bytes // self.sector_size

    @property
    def first_usable_lba(self) -> int:
        return 2 + self.table_sectors

    @property
    def last_usable_lba(self) -> int:
        return self.sector_count - self.table_sectors - 2


def require_int(owner: dict[str, object], key: str) -> int:
    value = owner.get(key)
    if isinstance(value, bool) or not isinstance(value, int):
        raise DiskError(f"{key} must be an integer")
    return value


def require_string(owner: dict[str, object], key: str) -> str:
    value = owner.get(key)
    if not isinstance(value, str) or not value:
        raise DiskError(f"{key} must be a non-empty string")
    return value


def parse_guid(owner: dict[str, object], key: str) -> uuid.UUID:
    try:
        return uuid.UUID(require_string(owner, key))
    except ValueError as error:
        raise DiskError(f"{key} is not a canonical UUID") from error


def load_layout(path: Path) -> Layout:
    try:
        document = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise DiskError(f"cannot read layout {path}: {error}") from error
    if not isinstance(document, dict):
        raise DiskError("layout root must be an object")
    allowed_root = {
        "schema_version",
        "sector_size",
        "alignment_bytes",
        "disk_size_bytes",
        "partition_entry_count",
        "disk_guid",
        "partitions",
    }
    unknown_root = set(document) - allowed_root
    if unknown_root:
        raise DiskError(f"unknown layout keys: {', '.join(sorted(unknown_root))}")
    if require_int(document, "schema_version") != 1:
        raise DiskError("only partition layout schema version 1 is supported")
    sector_size = require_int(document, "sector_size")
    alignment = require_int(document, "alignment_bytes")
    disk_size = require_int(document, "disk_size_bytes")
    entry_count = require_int(document, "partition_entry_count")
    if sector_size != 512:
        raise DiskError("GPT sector size must be exactly 512 bytes")
    if alignment < 1024 * 1024 or alignment % sector_size:
        raise DiskError("partition alignment must be a sector-aligned MiB or larger")
    if disk_size <= 0 or disk_size % sector_size:
        raise DiskError("disk size must be a positive whole number of sectors")
    if entry_count < 1 or entry_count > 4096:
        raise DiskError("partition entry count is outside the supported range")
    if entry_count * GPT_ENTRY_SIZE % sector_size:
        raise DiskError("partition entry array must occupy whole sectors")

    entries = document.get("partitions")
    if not isinstance(entries, list) or not entries:
        raise DiskError("partitions must be a non-empty array")
    if len(entries) > entry_count:
        raise DiskError("layout has more partitions than GPT entries")
    allowed_partition = {
        "name",
        "start_bytes",
        "size_bytes",
        "image",
        "type_guid",
        "unique_guid",
        "writable",
    }
    partitions: list[Partition] = []
    names: set[str] = set()
    images: set[str] = set()
    guids: set[uuid.UUID] = set()
    previous_end = 0
    for index, item in enumerate(entries, 1):
        if not isinstance(item, dict):
            raise DiskError(f"partition {index} must be an object")
        unknown = set(item) - allowed_partition
        if unknown:
            raise DiskError(
                f"partition {index} has unknown keys: {', '.join(sorted(unknown))}"
            )
        name = require_string(item, "name")
        if re.fullmatch(r"[a-z][a-z0-9_]{0,35}", name) is None:
            raise DiskError(f"partition {index} has an unsafe GPT name: {name}")
        image = require_string(item, "image")
        if Path(image).name != image or "/" in image or "\\" in image:
            raise DiskError(f"partition {name} image must be a plain filename")
        start = require_int(item, "start_bytes")
        size = require_int(item, "size_bytes")
        writable = item.get("writable")
        if not isinstance(writable, bool):
            raise DiskError(f"partition {name} writable must be boolean")
        if start % alignment or size <= 0 or size % alignment:
            raise DiskError(f"partition {name} is not aligned to {alignment} bytes")
        if start < previous_end:
            raise DiskError(f"partition {name} overlaps or is out of order")
        if name in names or image in images:
            raise DiskError(f"partition {name} duplicates a name or image")
        type_guid = parse_guid(item, "type_guid")
        unique_guid = parse_guid(item, "unique_guid")
        if unique_guid in guids:
            raise DiskError(f"partition {name} duplicates a unique GUID")
        partitions.append(
            Partition(
                name,
                start,
                size,
                image,
                type_guid,
                unique_guid,
                writable,
            )
        )
        names.add(name)
        images.add(image)
        guids.add(unique_guid)
        previous_end = start + size

    layout = Layout(
        sector_size,
        alignment,
        disk_size,
        entry_count,
        parse_guid(document, "disk_guid"),
        tuple(partitions),
    )
    minimum_start = layout.first_usable_lba * sector_size
    maximum_end = (layout.last_usable_lba + 1) * sector_size
    if partitions[0].start_bytes < minimum_start:
        raise DiskError("first partition overlaps the primary GPT")
    if previous_end > maximum_end:
        raise DiskError("last partition overlaps the backup GPT")
    return layout


def partition_entries(layout: Layout) -> bytes:
    table = bytearray(layout.table_bytes)
    for index, partition in enumerate(layout.partitions):
        name = partition.name.encode("utf-16-le")
        if len(name) > 72:
            raise DiskError(f"partition name is too long for GPT: {partition.name}")
        first_lba = partition.start_bytes // layout.sector_size
        last_lba = (
            partition.start_bytes + partition.size_bytes
        ) // layout.sector_size - 1
        entry = struct.pack(
            "<16s16sQQQ72s",
            partition.type_guid.bytes_le,
            partition.unique_guid.bytes_le,
            first_lba,
            last_lba,
            0,
            name.ljust(72, b"\0"),
        )
        offset = index * GPT_ENTRY_SIZE
        table[offset : offset + GPT_ENTRY_SIZE] = entry
    return bytes(table)


def gpt_header(
    layout: Layout,
    *,
    current_lba: int,
    backup_lba: int,
    entries_lba: int,
    entries_crc: int,
) -> bytes:
    header = bytearray(layout.sector_size)
    struct.pack_into(
        "<8sIIIIQQQQ16sQIII",
        header,
        0,
        GPT_SIGNATURE,
        GPT_REVISION,
        GPT_HEADER_SIZE,
        0,
        0,
        current_lba,
        backup_lba,
        layout.first_usable_lba,
        layout.last_usable_lba,
        layout.disk_guid.bytes_le,
        entries_lba,
        layout.partition_entry_count,
        GPT_ENTRY_SIZE,
        entries_crc,
    )
    crc = binascii.crc32(header[:GPT_HEADER_SIZE]) & 0xFFFFFFFF
    struct.pack_into("<I", header, 16, crc)
    return bytes(header)


def protective_mbr(layout: Layout) -> bytes:
    mbr = bytearray(layout.sector_size)
    sectors = min(layout.sector_count - 1, 0xFFFFFFFF)
    struct.pack_into(
        "<B3sB3sII",
        mbr,
        446,
        0,
        b"\0\x02\0",
        PROTECTIVE_MBR_TYPE,
        b"\xff\xff\xff",
        1,
        sectors,
    )
    mbr[510:512] = b"\x55\xaa"
    return bytes(mbr)


def copy_exact(source: BinaryIO, destination: BinaryIO, byte_count: int) -> None:
    remaining = byte_count
    while remaining:
        block = source.read(min(remaining, COPY_CHUNK))
        if not block:
            raise DiskError("sparse RAW chunk is truncated")
        destination.write(block)
        remaining -= len(block)


def write_fill(destination: BinaryIO, pattern: bytes, byte_count: int) -> None:
    if pattern == b"\0\0\0\0":
        destination.seek(byte_count, os.SEEK_CUR)
        return
    block = pattern * (COPY_CHUNK // len(pattern))
    remaining = byte_count
    while remaining:
        piece = block[: min(remaining, len(block))]
        destination.write(piece)
        remaining -= len(piece)


def expand_sparse(source: BinaryIO, destination: BinaryIO, maximum: int) -> int:
    raw_header = source.read(SPARSE_HEADER.size)
    if len(raw_header) != SPARSE_HEADER.size:
        raise DiskError("Android sparse header is truncated")
    (
        magic,
        major,
        _minor,
        file_header_size,
        chunk_header_size,
        block_size,
        total_blocks,
        total_chunks,
        _checksum,
    ) = SPARSE_HEADER.unpack(raw_header)
    if magic != SPARSE_MAGIC or major != 1:
        raise DiskError("unsupported Android sparse image version")
    if file_header_size < SPARSE_HEADER.size or chunk_header_size < SPARSE_CHUNK_HEADER.size:
        raise DiskError("Android sparse header sizes are invalid")
    if block_size == 0 or block_size % 4:
        raise DiskError("Android sparse block size is invalid")
    expanded_size = total_blocks * block_size
    if expanded_size > maximum:
        raise DiskError(
            f"expanded Android sparse image is {expanded_size} bytes, limit is {maximum}"
        )
    source.seek(file_header_size - SPARSE_HEADER.size, os.SEEK_CUR)
    written_blocks = 0
    for _ in range(total_chunks):
        raw_chunk = source.read(SPARSE_CHUNK_HEADER.size)
        if len(raw_chunk) != SPARSE_CHUNK_HEADER.size:
            raise DiskError("Android sparse chunk header is truncated")
        chunk_type, _reserved, chunk_blocks, total_size = SPARSE_CHUNK_HEADER.unpack(
            raw_chunk
        )
        source.seek(chunk_header_size - SPARSE_CHUNK_HEADER.size, os.SEEK_CUR)
        chunk_bytes = chunk_blocks * block_size
        data_size = total_size - chunk_header_size
        if total_size < chunk_header_size:
            raise DiskError("Android sparse chunk size underflows its header")
        if written_blocks + chunk_blocks > total_blocks:
            raise DiskError("Android sparse chunks exceed declared block count")
        if chunk_type == SPARSE_RAW:
            if data_size != chunk_bytes:
                raise DiskError("Android sparse RAW chunk has the wrong byte count")
            copy_exact(source, destination, chunk_bytes)
        elif chunk_type == SPARSE_FILL:
            if data_size != 4:
                raise DiskError("Android sparse FILL chunk must contain four bytes")
            pattern = source.read(4)
            if len(pattern) != 4:
                raise DiskError("Android sparse FILL chunk is truncated")
            write_fill(destination, pattern, chunk_bytes)
        elif chunk_type == SPARSE_DONT_CARE:
            if data_size != 0:
                raise DiskError("Android sparse DONT_CARE chunk contains data")
            destination.seek(chunk_bytes, os.SEEK_CUR)
        elif chunk_type == SPARSE_CRC32:
            if chunk_blocks != 0 or data_size != 4:
                raise DiskError("Android sparse CRC32 chunk is malformed")
            if len(source.read(4)) != 4:
                raise DiskError("Android sparse CRC32 chunk is truncated")
        else:
            raise DiskError(f"unknown Android sparse chunk type 0x{chunk_type:04x}")
        written_blocks += chunk_blocks
    if written_blocks != total_blocks:
        raise DiskError("Android sparse image block count does not match its chunks")
    if source.read(1):
        raise DiskError("Android sparse image has trailing data")
    return expanded_size


def write_partition_image(image: Path, destination: BinaryIO, maximum: int) -> int:
    if image.is_symlink() or not image.is_file():
        raise DiskError(f"partition image must be a regular non-symlink file: {image}")
    with image.open("rb") as source:
        prefix = source.read(4)
        source.seek(0)
        if len(prefix) == 4 and struct.unpack("<I", prefix)[0] == SPARSE_MAGIC:
            return expand_sparse(source, destination, maximum)
        size = image.stat().st_size
        if size > maximum:
            raise DiskError(f"raw image {image.name} is {size} bytes, limit is {maximum}")
        copy_exact(source, destination, size)
        return size


def assemble(layout: Layout, images_dir: Path, output: Path) -> None:
    if images_dir.is_symlink() or not images_dir.is_dir():
        raise DiskError(f"images directory must be a non-symlink directory: {images_dir}")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".{output.name}.tmp.{os.getpid()}")
    if temporary.exists():
        raise DiskError(f"temporary output already exists: {temporary}")
    table = partition_entries(layout)
    table_crc = binascii.crc32(table) & 0xFFFFFFFF
    primary_header = gpt_header(
        layout,
        current_lba=1,
        backup_lba=layout.sector_count - 1,
        entries_lba=2,
        entries_crc=table_crc,
    )
    backup_entries_lba = layout.sector_count - layout.table_sectors - 1
    backup_header = gpt_header(
        layout,
        current_lba=layout.sector_count - 1,
        backup_lba=1,
        entries_lba=backup_entries_lba,
        entries_crc=table_crc,
    )
    try:
        with temporary.open("xb") as disk:
            disk.truncate(layout.disk_size_bytes)
            disk.seek(0)
            disk.write(protective_mbr(layout))
            disk.write(primary_header)
            disk.write(table)
            disk.seek(backup_entries_lba * layout.sector_size)
            disk.write(table)
            disk.seek((layout.sector_count - 1) * layout.sector_size)
            disk.write(backup_header)
            for partition in layout.partitions:
                disk.seek(partition.start_bytes)
                image = images_dir / partition.image
                used = write_partition_image(image, disk, partition.size_bytes)
                print(
                    f"staged {partition.name}: {used}/{partition.size_bytes} bytes "
                    f"at {partition.start_bytes}"
                )
            disk.flush()
            os.fsync(disk.fileno())
        os.replace(temporary, output)
    except BaseException:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise


def read_exact(source: BinaryIO, offset: int, size: int) -> bytes:
    source.seek(offset)
    payload = source.read(size)
    if len(payload) != size:
        raise DiskError("disk image is truncated")
    return payload


def verify_header(
    layout: Layout,
    sector: bytes,
    *,
    current_lba: int,
    backup_lba: int,
    entries_lba: int,
    entries_crc: int,
) -> None:
    if sector[:8] != GPT_SIGNATURE:
        raise DiskError("GPT header signature is missing")
    revision, size, recorded_crc, reserved = struct.unpack_from("<IIII", sector, 8)
    if revision != GPT_REVISION or size != GPT_HEADER_SIZE or reserved != 0:
        raise DiskError("GPT header revision, size, or reserved field is invalid")
    header = bytearray(sector[:size])
    struct.pack_into("<I", header, 16, 0)
    if binascii.crc32(header) & 0xFFFFFFFF != recorded_crc:
        raise DiskError("GPT header CRC32 does not match")
    fields = struct.unpack_from("<QQQQ16sQIII", sector, 24)
    expected = (
        current_lba,
        backup_lba,
        layout.first_usable_lba,
        layout.last_usable_lba,
        layout.disk_guid.bytes_le,
        entries_lba,
        layout.partition_entry_count,
        GPT_ENTRY_SIZE,
        entries_crc,
    )
    if fields != expected:
        raise DiskError("GPT header does not match the checked layout")


def verify(layout: Layout, disk_path: Path) -> None:
    if disk_path.is_symlink() or not disk_path.is_file():
        raise DiskError(f"disk image must be a regular non-symlink file: {disk_path}")
    if disk_path.stat().st_size != layout.disk_size_bytes:
        raise DiskError("disk image size does not match the checked layout")
    expected_table = partition_entries(layout)
    table_crc = binascii.crc32(expected_table) & 0xFFFFFFFF
    backup_entries_lba = layout.sector_count - layout.table_sectors - 1
    with disk_path.open("rb") as disk:
        mbr = read_exact(disk, 0, layout.sector_size)
        if mbr[510:512] != b"\x55\xaa" or mbr[450] != PROTECTIVE_MBR_TYPE:
            raise DiskError("protective MBR is invalid")
        primary = read_exact(disk, layout.sector_size, layout.sector_size)
        primary_table = read_exact(disk, 2 * layout.sector_size, layout.table_bytes)
        backup_table = read_exact(
            disk, backup_entries_lba * layout.sector_size, layout.table_bytes
        )
        backup = read_exact(
            disk, (layout.sector_count - 1) * layout.sector_size, layout.sector_size
        )
    if primary_table != expected_table or backup_table != expected_table:
        raise DiskError("GPT entry array does not match the checked layout")
    if binascii.crc32(primary_table) & 0xFFFFFFFF != table_crc:
        raise DiskError("GPT entry-array CRC32 does not match")
    verify_header(
        layout,
        primary,
        current_lba=1,
        backup_lba=layout.sector_count - 1,
        entries_lba=2,
        entries_crc=table_crc,
    )
    verify_header(
        layout,
        backup,
        current_lba=layout.sector_count - 1,
        backup_lba=1,
        entries_lba=backup_entries_lba,
        entries_crc=table_crc,
    )
    print(
        f"verified UBD GPT disk: {len(layout.partitions)} partitions, "
        f"{layout.disk_size_bytes} bytes"
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Assemble or verify the fixed Android 17 raw UBD GPT disk"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate_parser = subparsers.add_parser("validate")
    validate_parser.add_argument("--layout", type=Path, required=True)
    assemble_parser = subparsers.add_parser("assemble")
    assemble_parser.add_argument("--layout", type=Path, required=True)
    assemble_parser.add_argument("--images-dir", type=Path, required=True)
    assemble_parser.add_argument("--output", type=Path, required=True)
    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("--layout", type=Path, required=True)
    verify_parser.add_argument("--disk", type=Path, required=True)
    args = parser.parse_args()
    try:
        layout = load_layout(args.layout.resolve())
        if args.command == "assemble":
            assemble(layout, args.images_dir.resolve(), args.output.resolve())
            verify(layout, args.output.resolve())
        elif args.command == "verify":
            verify(layout, args.disk.resolve())
        else:
            print(
                f"validated UBD GPT layout: {len(layout.partitions)} partitions, "
                f"{layout.disk_size_bytes} bytes"
            )
    except (DiskError, OSError) as error:
        raise SystemExit(f"UBD disk: {error}") from error


if __name__ == "__main__":
    main()
