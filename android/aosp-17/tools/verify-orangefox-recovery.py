#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify the kernel-less OrangeFox recovery partition and compressed ramdisk."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import gzip
import io
import json
import stat
import struct
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import BinaryIO


BOOT_MAGIC = b"ANDROID!"
BOOT_HEADER_SIZE = 1584
BOOT_PAGE_SIZE = 4096
MAX_RAMDISK_SIZE = 256 * 1024 * 1024
MAX_ENTRY_SIZE = 128 * 1024 * 1024
NEWC_HEADER_SIZE = 110
ELF_MAGIC = b"\x7fELF"
EM_AARCH64 = 183
PT_LOAD = 1
PT_DYNAMIC = 2
DT_NULL = 0
DT_NEEDED = 1
DT_STRTAB = 5
DT_STRSZ = 10
PAGE_SIZE = 16384
REQUIRED_PATHS = {
    "init",
    "prop.default",
    "system/bin/recovery",
    "system/etc/fox.cfg",
    "system/etc/recovery.fstab",
    "twres/pages/recovery.xml",
    "twres/pages/backup.xml",
    "twres/pages/restore.xml",
    "twres/pages/files.xml",
    "twres/pages/mount.xml",
    "twres/pages/wipe.xml",
}
FORBIDDEN_MODULE_METADATA = {
    "modules.alias", "modules.alias.bin", "modules.builtin",
    "modules.builtin.modinfo", "modules.dep", "modules.dep.bin",
    "modules.load", "modules.options", "modules.order", "modules.softdep",
}


class VerificationError(ValueError):
    pass


@dataclass(frozen=True)
class Entry:
    mode: int
    payload: bytes


def fail(message: str) -> None:
    raise VerificationError(message)


def regular_file(path: Path, label: str) -> Path:
    if path.is_symlink() or not path.is_file():
        fail(f"{label} must be a regular non-symlink file: {path}")
    return path.resolve()


def partition_range(layout_path: Path, disk_path: Path) -> tuple[int, int]:
    try:
        layout = json.loads(layout_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"cannot read partition layout: {error}")
    partitions = layout.get("partitions") if isinstance(layout, dict) else None
    matches = [
        item for item in partitions or []
        if isinstance(item, dict) and item.get("name") == "recovery"
    ]
    if len(matches) != 1:
        fail("partition layout does not contain one recovery partition")
    offset = matches[0].get("start_bytes")
    size = matches[0].get("size_bytes")
    disk_size = layout.get("disk_size_bytes")
    if (
        isinstance(offset, bool) or not isinstance(offset, int) or offset < 0
        or isinstance(size, bool) or not isinstance(size, int) or size <= 0
        or isinstance(disk_size, bool) or not isinstance(disk_size, int)
        or disk_path.stat().st_size != disk_size or offset + size > disk_size
    ):
        fail("recovery partition range is invalid")
    return offset, size


def read_exact(source: BinaryIO, count: int, label: str) -> bytes:
    payload = source.read(count)
    if len(payload) != count:
        fail(f"truncated {label}")
    return payload


def boot_ramdisk(source: BinaryIO, offset: int, container_size: int) -> bytes:
    if offset < 0 or container_size < BOOT_PAGE_SIZE:
        fail("boot image container is invalid")
    source.seek(offset)
    header = read_exact(source, BOOT_PAGE_SIZE, "boot header")
    if header[:8] != BOOT_MAGIC:
        fail("Android boot magic is missing")
    kernel_size, ramdisk_size, _os_version, header_size = struct.unpack_from(
        "<IIII", header, 8
    )
    header_version = struct.unpack_from("<I", header, 40)[0]
    signature_size = struct.unpack_from("<I", header, 1580)[0]
    if header_version != 4 or header_size != BOOT_HEADER_SIZE:
        fail("recovery must use the exact boot-image-v4 header")
    if kernel_size != 0:
        fail("recovery embeds the independently signed ACK kernel")
    if ramdisk_size <= 0 or ramdisk_size > container_size - BOOT_PAGE_SIZE:
        fail("recovery ramdisk size is invalid")
    if signature_size != 0 or any(header[BOOT_HEADER_SIZE:]):
        fail("recovery carries a signature or nonzero header padding")
    return read_exact(source, ramdisk_size, "compressed recovery ramdisk")


def decompress_ramdisk(payload: bytes) -> bytes:
    if not payload.startswith(b"\x1f\x8b"):
        fail("OrangeFox recovery ramdisk is not gzip-compressed")
    try:
        with gzip.GzipFile(fileobj=io.BytesIO(payload), mode="rb") as archive:
            result = archive.read(MAX_RAMDISK_SIZE + 1)
            if archive.read(1):
                fail("decompressed recovery ramdisk exceeds the size limit")
    except (OSError, EOFError) as error:
        fail(f"cannot decompress recovery ramdisk: {error}")
    if not result or len(result) > MAX_RAMDISK_SIZE:
        fail("decompressed recovery ramdisk size is invalid")
    return result


def align4(value: int) -> int:
    return (value + 3) & ~3


def safe_name(raw: bytes) -> str:
    try:
        name = raw.decode("utf-8", errors="strict")
    except UnicodeError as error:
        fail(f"newc path is not UTF-8: {error}")
    path = PurePosixPath(name)
    if (
        not name or name.startswith("/") or "\\" in name or str(path) != name
        or any(part in ("", ".", "..") for part in path.parts)
    ):
        fail(f"unsafe newc path: {name!r}")
    return name


def parse_newc(payload: bytes) -> dict[str, Entry]:
    entries: dict[str, Entry] = {}
    position = 0
    while True:
        if position + NEWC_HEADER_SIZE > len(payload):
            fail("truncated newc header")
        header = payload[position : position + NEWC_HEADER_SIZE]
        position += NEWC_HEADER_SIZE
        if header[:6] not in (b"070701", b"070702"):
            fail("recovery ramdisk is not a newc archive")
        try:
            fields = [
                int(header[field : field + 8], 16)
                for field in range(6, NEWC_HEADER_SIZE, 8)
            ]
        except ValueError as error:
            fail(f"malformed newc header: {error}")
        mode = fields[1]
        file_size = fields[6]
        name_size = fields[11]
        if file_size > MAX_ENTRY_SIZE or name_size < 2 or name_size > 4096:
            fail("newc entry size is invalid")
        end_name = position + name_size
        if end_name > len(payload) or payload[end_name - 1] != 0:
            fail("truncated or unterminated newc path")
        raw_name = payload[position : end_name - 1]
        position = align4(end_name)
        if raw_name == b"TRAILER!!!":
            if file_size != 0 or any(payload[position:]):
                fail("newc trailer has data")
            break
        name = safe_name(raw_name)
        end_data = position + file_size
        if end_data > len(payload):
            fail(f"truncated newc payload: {name}")
        entry = Entry(mode, payload[position:end_data])
        position = align4(end_data)
        previous = entries.get(name)
        if previous is not None:
            duplicate_directory = (
                stat.S_ISDIR(previous.mode) and stat.S_ISDIR(mode)
                and stat.S_IMODE(previous.mode) == stat.S_IMODE(mode)
                and not previous.payload and not entry.payload
            )
            if not duplicate_directory:
                fail(f"ambiguous duplicate newc entry: {name}")
        entries[name] = entry
    return entries


def verify_elf(name: str, payload: bytes) -> bool:
    if not payload.startswith(ELF_MAGIC):
        return False
    if len(payload) < 64 or payload[4] != 2 or payload[5] != 1:
        fail(f"target ELF is not little-endian ELF64: {name}")
    machine = struct.unpack_from("<H", payload, 18)[0]
    if machine != EM_AARCH64:
        fail(f"target ELF is not AArch64: {name}")
    program_offset = struct.unpack_from("<Q", payload, 32)[0]
    entry_size, entry_count = struct.unpack_from("<HH", payload, 54)
    if entry_size < 56 or program_offset + entry_size * entry_count > len(payload):
        fail(f"target ELF program headers are malformed: {name}")
    loads = 0
    for index in range(entry_count):
        values = struct.unpack_from(
            "<IIQQQQQQ", payload, program_offset + index * entry_size
        )
        kind, _flags, file_offset, virtual_address, _physical_address, file_size, _memory_size, alignment = values
        if file_offset + file_size > len(payload):
            fail(f"target ELF segment exceeds its payload: {name}")
        if kind != PT_LOAD:
            continue
        loads += 1
        if alignment < PAGE_SIZE or alignment & (alignment - 1):
            fail(f"target ELF load alignment is below 16 KiB: {name} ({alignment:#x})")
        if virtual_address % alignment != file_offset % alignment:
            fail(f"target ELF load alignment is incongruent: {name}")
    if loads == 0:
        fail(f"target ELF contains no load segment: {name}")
    return True


def elf_needed(name: str, payload: bytes) -> set[str]:
    program_offset = struct.unpack_from("<Q", payload, 32)[0]
    entry_size, entry_count = struct.unpack_from("<HH", payload, 54)
    loads: list[tuple[int, int, int]] = []
    dynamic: tuple[int, int] | None = None
    for index in range(entry_count):
        offset = program_offset + index * entry_size
        (
            kind, _flags, file_offset, virtual_address, _physical_address,
            file_size, _memory_size, _alignment,
        ) = struct.unpack_from("<IIQQQQQQ", payload, offset)
        if kind == PT_LOAD:
            loads.append((file_offset, virtual_address, file_size))
        elif kind == PT_DYNAMIC:
            if dynamic is not None:
                fail(f"target ELF has multiple dynamic segments: {name}")
            dynamic = (file_offset, file_size)
    if dynamic is None:
        return set()

    dynamic_offset, dynamic_size = dynamic
    if dynamic_size % 16:
        fail(f"target ELF dynamic segment is malformed: {name}")
    string_address: int | None = None
    string_size: int | None = None
    needed_offsets: list[int] = []
    terminated = False
    for offset in range(dynamic_offset, dynamic_offset + dynamic_size, 16):
        tag, value = struct.unpack_from("<qQ", payload, offset)
        if tag == DT_NULL:
            terminated = True
            break
        if tag == DT_NEEDED:
            needed_offsets.append(value)
        elif tag == DT_STRTAB:
            string_address = value
        elif tag == DT_STRSZ:
            string_size = value
    if not terminated:
        fail(f"target ELF dynamic segment lacks DT_NULL: {name}")
    if not needed_offsets:
        return set()
    if string_address is None or string_size is None:
        fail(f"target ELF dynamic string table is incomplete: {name}")

    string_offset: int | None = None
    for file_offset, virtual_address, file_size in loads:
        if virtual_address <= string_address < virtual_address + file_size:
            string_offset = file_offset + string_address - virtual_address
            break
    if string_offset is None or string_offset + string_size > len(payload):
        fail(f"target ELF dynamic string table is outside a load segment: {name}")
    strings = payload[string_offset : string_offset + string_size]
    needed: set[str] = set()
    for offset in needed_offsets:
        if offset >= len(strings):
            fail(f"target ELF DT_NEEDED offset is out of bounds: {name}")
        end = strings.find(b"\0", offset)
        if end < 0:
            fail(f"target ELF DT_NEEDED string is unterminated: {name}")
        try:
            library = strings[offset:end].decode("utf-8", errors="strict")
        except UnicodeError as error:
            fail(f"target ELF DT_NEEDED string is not UTF-8: {name}: {error}")
        if not library or "/" in library or library in (".", ".."):
            fail(f"target ELF has an unsafe DT_NEEDED name: {name}")
        needed.add(library)
    return needed


def verify_entries(entries: dict[str, Entry], expected_fstab: bytes) -> int:
    missing = sorted(REQUIRED_PATHS - entries.keys())
    if missing:
        fail(f"OrangeFox ramdisk is missing required paths: {missing}")
    recovery = entries["system/bin/recovery"]
    if not stat.S_ISREG(recovery.mode) or not recovery.mode & 0o111:
        fail("OrangeFox recovery binary is not an executable regular file")
    if entries["system/etc/recovery.fstab"].payload != expected_fstab:
        fail("OrangeFox recovery fstab differs from the checked device tree")
    properties = entries["prop.default"].payload
    for token in (
        b"ro.product.system.device=darwin",
        b"ro.product.system.model=OrangeFox on Darwin UML",
        b"service.adb.tcp.port=5555",
    ):
        if token not in properties:
            fail(f"OrangeFox properties omit {token.decode()}")
    if b"FOX_CODE_BASE=" not in entries["system/etc/fox.cfg"].payload:
        fail("OrangeFox build identity is missing")

    elf_count = 0
    needed_by: dict[str, set[str]] = {}
    available_libraries: set[str] = set()
    for name, entry in entries.items():
        basename = PurePosixPath(name).name.lower()
        if basename.endswith(".ko") or ".ko." in basename or basename in FORBIDDEN_MODULE_METADATA:
            fail(f"loadable-module payload remains in OrangeFox: {name}")
        if stat.S_ISREG(entry.mode) and verify_elf(name, entry.payload):
            elf_count += 1
            needed_by[name] = elf_needed(name, entry.payload)
            path = PurePosixPath(name)
            if path.parent.name in ("lib", "lib64"):
                available_libraries.add(path.name)
    if elf_count == 0:
        fail("OrangeFox ramdisk contains no target ELF payloads")
    for name, needed in needed_by.items():
        missing_libraries = sorted(needed - available_libraries)
        if missing_libraries:
            fail(
                f"OrangeFox ELF dependency closure is incomplete for {name}: "
                f"{missing_libraries}"
            )
    return elf_count


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--android-root", type=Path, required=True)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--image", type=Path)
    source.add_argument("--disk", type=Path)
    parser.add_argument("--layout", type=Path)
    args = parser.parse_args()
    try:
        android_root = args.android_root.resolve()
        fstab_path = android_root / "third_party/orangefox/device/recovery/root/system/etc/recovery.fstab"
        expected_fstab = regular_file(fstab_path, "checked OrangeFox fstab").read_bytes()
        if args.image is not None:
            if args.layout is not None:
                fail("--layout is valid only with --disk")
            path = regular_file(args.image, "OrangeFox recovery image")
            offset, size = 0, path.stat().st_size
        else:
            if args.layout is None:
                fail("--disk requires --layout")
            path = regular_file(args.disk, "Android disk image")
            layout = regular_file(args.layout, "partition layout")
            offset, size = partition_range(layout, path)
        with path.open("rb") as image:
            compressed = boot_ramdisk(image, offset, size)
        entries = parse_newc(decompress_ramdisk(compressed))
        elf_count = verify_entries(entries, expected_fstab)
    except (OSError, UnicodeError, struct.error, VerificationError) as error:
        raise SystemExit(f"OrangeFox recovery: {error}") from error
    print(
        f"verified kernel-less OrangeFox recovery: {len(entries)} entries, "
        f"{elf_count} aligned AArch64 ELFs"
    )


if __name__ == "__main__":
    main()
