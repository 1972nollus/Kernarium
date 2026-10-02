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
from pathlib import Path, PurePosixPath

import android_image


MAX_RAMDISK_SIZE = 256 * 1024 * 1024
ELF_MAGIC = b"\x7fELF"
EM_AARCH64 = 183
REQUIRED_PATHS = {
    "init",
    "prop.default",
    "system/bin/blockdev",
    "system/bin/e2fsck",
    "system/bin/kernarium-grow-userdata",
    "system/bin/recovery",
    "system/bin/resize2fs",
    "system/bin/tune2fs",
    "system/etc/cgroups.json",
    "system/etc/fox.cfg",
    "system/etc/recovery.fstab",
    "system/etc/task_profiles.json",
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


def fail(message: str) -> None:
    raise VerificationError(message)


def regular_file(path: Path, label: str) -> Path:
    if path.is_symlink() or not path.is_file():
        fail(f"{label} must be a regular non-symlink file: {path}")
    return path.resolve()


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
    recovery_hardware_init = entries.get("init.recovery.cutf_cvm.rc")
    if recovery_hardware_init is None:
        fail("OrangeFox ramdisk omits the UML recovery hardware init script")
    for token in (
        b"/system/bin/ifconfig eth0 10.0.2.15",
        b"netmask 255.255.255.0 up",
        b"seclabel u:r:su:s0",
        b"exec u:r:su:s0 root root -- /system/bin/sh "
        b"/system/bin/kernarium-grow-userdata",
    ):
        if token not in recovery_hardware_init.payload:
            fail(f"OrangeFox recovery hardware setup omits {token.decode()}")
    grow_userdata = entries["system/bin/kernarium-grow-userdata"].payload
    for token in (
        b"blockdev --getsz \"$partition\"",
        b"tune2fs -l",
        b"filesystem_sectors=$((filesystem_blocks * filesystem_block_sectors))",
        b"e2fsck -pf",
        b"resize2fs",
    ):
        if token not in grow_userdata:
            fail(f"OrangeFox userdata expansion omits {token.decode()}")
    if b"FOX_CODE_BASE=" not in entries["system/etc/fox.cfg"].payload:
        fail("OrangeFox build identity is missing")
    if "system/lib64/libtwrpmtp-ffs.so" in entries:
        fail("OrangeFox retains unsupported USB MTP support")
    if "system/bin/keystore_cli_v2" in entries:
        fail("OrangeFox retains the unsupported KeyMint command-line client")

    try:
        cgroups = json.loads(entries["system/etc/cgroups.json"].payload)
        task_profiles = json.loads(entries["system/etc/task_profiles.json"].payload)
    except (UnicodeError, json.JSONDecodeError) as error:
        fail(f"OrangeFox process-group configuration is invalid: {error}")
    controllers = {
        item.get("Controller")
        for item in cgroups.get("Cgroups", [])
        if isinstance(item, dict)
    } if isinstance(cgroups, dict) else set()
    profiles = {
        item.get("Name")
        for item in task_profiles.get("Profiles", [])
        if isinstance(item, dict)
    } if isinstance(task_profiles, dict) else set()
    if "cpuset" not in controllers or "ServiceCapacityLow" not in profiles:
        fail("OrangeFox process-group configuration cannot start recovery logd")

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
        raise SystemExit(f"OrangeFox recovery: {error}") from error
    print(
        f"verified kernel-less OrangeFox recovery: {len(entries)} entries, "
        f"{elf_count} aligned AArch64 ELFs"
    )


if __name__ == "__main__":
    main()
