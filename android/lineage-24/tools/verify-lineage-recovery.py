#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify the kernel-less LineageOS recovery partition and its compressed ramdisk."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import gzip
import io
import stat
import struct
from pathlib import Path, PurePosixPath

import android_image


MAX_RAMDISK_SIZE = 256 * 1024 * 1024
LZ4_LEGACY_MAGIC = b"\x02\x21\x4c\x18"
LZ4_LEGACY_BLOCK_LIMIT = 8 * 1024 * 1024
ELF_MAGIC = b"\x7fELF"
EM_AARCH64 = 183
REQUIRED_PATHS = {
    "init",
    "prop.default",
    "system/bin/adbd",
    "system/bin/e2fsck",
    "system/bin/mke2fs",
    "system/bin/recovery",
    "system/bin/resize2fs",
    "system/bin/tune2fs",
    "system/etc/recovery.fstab",
    "system/lib64/librecovery_ui.so",
    "system/lib64/librecovery_ui_ext.so",
}
FORBIDDEN_MODULE_METADATA = {
    "modules.alias", "modules.alias.bin", "modules.builtin",
    "modules.builtin.modinfo", "modules.dep", "modules.dep.bin",
    "modules.load", "modules.options", "modules.order", "modules.softdep",
}


class VerificationError(ValueError):
    pass


def fail(message: str) -> None:
    raise VerificationError(message)


def regular_file(path: Path, label: str) -> Path:
    if path.is_symlink() or not path.is_file():
        fail(f"{label} must be a regular non-symlink file: {path}")
    return path.resolve()


def lz4_block(block: bytes, limit: int) -> bytes:
    """Decode one LZ4 block into at most `limit` bytes."""
    output = bytearray()
    position, length = 0, len(block)
    try:
        while position < length:
            token = block[position]
            position += 1
            literals = token >> 4
            if literals == 15:
                while True:
                    extra = block[position]
                    position += 1
                    literals += extra
                    if extra != 255:
                        break
            if position + literals > length:
                fail("LZ4 literal run exceeds its block")
            output += block[position:position + literals]
            position += literals
            if position == length:
                break
            offset = block[position] | (block[position + 1] << 8)
            position += 2
            if offset == 0 or offset > len(output):
                fail("LZ4 match offset is outside the decoded data")
            match = token & 15
            if match == 15:
                while True:
                    extra = block[position]
                    position += 1
                    match += extra
                    if extra != 255:
                        break
            match += 4
            start = len(output) - offset
            if offset >= match:
                output += output[start:start + match]
            else:
                pattern = bytes(output[start:])
                output += (pattern * (match // offset + 1))[:match]
            if len(output) > limit:
                fail("LZ4 block exceeds its size limit")
    except IndexError:
        fail("LZ4 block is truncated")
    if len(output) > limit:
        fail("LZ4 block exceeds its size limit")
    return bytes(output)


def decompress_lz4_legacy(payload: bytes) -> bytes:
    """Decode the LZ4 legacy frame that Android's recovery ramdisk uses."""
    if not payload.startswith(LZ4_LEGACY_MAGIC):
        fail("recovery ramdisk is not an LZ4 legacy frame")
    result = bytearray()
    position = 0
    while position < len(payload):
        word = payload[position:position + 4]
        if len(word) != 4:
            fail("LZ4 legacy frame has trailing bytes")
        position += 4
        if word == LZ4_LEGACY_MAGIC:
            continue
        size = struct.unpack("<I", word)[0]
        if size == 0 or position + size > len(payload):
            fail("LZ4 legacy block exceeds the ramdisk")
        result += lz4_block(payload[position:position + size], LZ4_LEGACY_BLOCK_LIMIT)
        position += size
        if len(result) > MAX_RAMDISK_SIZE:
            fail("decompressed recovery ramdisk exceeds the size limit")
    return bytes(result)


def decompress_ramdisk(payload: bytes) -> bytes:
    if payload.startswith(LZ4_LEGACY_MAGIC):
        result = decompress_lz4_legacy(payload)
    elif payload.startswith(b"\x1f\x8b"):
        try:
            with gzip.GzipFile(fileobj=io.BytesIO(payload), mode="rb") as archive:
                result = archive.read(MAX_RAMDISK_SIZE + 1)
        except (OSError, EOFError) as error:
            fail(f"cannot decompress recovery ramdisk: {error}")
    else:
        fail("recovery ramdisk is neither an LZ4 legacy frame nor gzip")
    if not result or len(result) > MAX_RAMDISK_SIZE:
        fail("decompressed recovery ramdisk size is invalid")
    return result


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
    for segment in android_image.program_headers(payload):
        if segment.offset + segment.file_size > len(payload):
            fail(f"target ELF segment exceeds its payload: {name}")
    if android_image.check_load_alignment(name, payload, VerificationError) == 0:
        fail(f"target ELF contains no load segment: {name}")
    return True


def elf_needed(name: str, payload: bytes) -> set[str]:
    needed = android_image.elf_needed(name, payload, VerificationError)
    if any(not library or "/" in library or library in (".", "..") for library in needed):
        fail(f"target ELF has an unsafe DT_NEEDED name: {name}")
    return needed


def verify_entries(
    entries: dict[str, android_image.NewcEntry], expected_fstab: bytes
) -> int:
    missing = sorted(REQUIRED_PATHS - entries.keys())
    if missing:
        fail(f"recovery ramdisk is missing required paths: {missing}")
    for name in ("system/bin/recovery", "system/bin/adbd"):
        entry = entries[name]
        if not stat.S_ISREG(entry.mode) or not entry.mode & 0o111:
            fail(f"recovery binary is not an executable regular file: {name}")
    if entries["system/etc/recovery.fstab"].payload != expected_fstab:
        fail("recovery fstab differs from the checked device tree")
    properties = entries["prop.default"].payload
    if b"persist.adb.tcp.port=5555" not in properties:
        fail("recovery properties omit the ADB TCP port")
    interface = entries["system/lib64/librecovery_ui_ext.so"].payload
    if b"eth1" in interface or any(
        token not in interface for token in (b"eth0\0", b"10.0.2.15\0", b"255.255.255.0\0")
    ):
        fail("recovery UI does not address the Virtio-net interface eth0")
    if "system/lib64/librecovery_ui_cuttlefish.so" in entries:
        fail("recovery retains the Cuttlefish interface policy")

    elf_count = 0
    needed_by: dict[str, set[str]] = {}
    available_libraries: set[str] = set()
    for name, entry in entries.items():
        basename = PurePosixPath(name).name.lower()
        if basename.endswith(".ko") or ".ko." in basename or basename in FORBIDDEN_MODULE_METADATA:
            fail(f"loadable-module payload remains in recovery: {name}")
        if stat.S_ISREG(entry.mode) and verify_elf(name, entry.payload):
            elf_count += 1
            needed_by[name] = elf_needed(name, entry.payload)
            path = PurePosixPath(name)
            if path.parent.name in ("lib", "lib64"):
                available_libraries.add(path.name)
    if elf_count == 0:
        fail("recovery ramdisk contains no target ELF payloads")
    for name, needed in needed_by.items():
        missing_libraries = sorted(needed - available_libraries)
        if missing_libraries:
            fail(
                f"recovery ELF dependency closure is incomplete for {name}: "
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
        fstab_path = android_root / "overlay/config/recovery.fstab"
        expected_fstab = regular_file(fstab_path, "checked recovery fstab").read_bytes()
        if args.image is not None:
            if args.layout is not None:
                fail("--layout is valid only with --disk")
            path = regular_file(args.image, "recovery image")
            offset, size = 0, path.stat().st_size
        else:
            if args.layout is None:
                fail("--disk requires --layout")
            path = regular_file(args.disk, "Android disk image")
            layout = regular_file(args.layout, "partition layout")
            offset, size = android_image.partition_range(
                layout, path, "recovery", VerificationError
            )
        with path.open("rb") as image:
            boot = android_image.parse_boot_image(image, offset, size, VerificationError)
            image.seek(boot.ramdisk_offset)
            compressed = android_image.read_exact(
                image, boot.ramdisk_size, "compressed recovery ramdisk", VerificationError
            )
        ramdisk = decompress_ramdisk(compressed)
        entries = android_image.parse_newc(
            android_image.RangeReader(
                io.BytesIO(ramdisk), 0, len(ramdisk), VerificationError
            ),
            lambda name, mode: True,
            VerificationError,
        )
        elf_count = verify_entries(entries, expected_fstab)
    except (OSError, UnicodeError, struct.error, VerificationError) as error:
        raise SystemExit(f"LineageOS recovery: {error}") from error
    print(
        f"verified kernel-less LineageOS recovery: {len(entries)} entries, "
        f"{elf_count} aligned AArch64 ELFs"
    )


if __name__ == "__main__":
    main()
