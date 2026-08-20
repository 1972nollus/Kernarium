#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import stat
from dataclasses import dataclass
from pathlib import Path, PurePosixPath


NEWC_HEADER_SIZE = 110
MAX_INITRAMFS_SIZE = 256 * 1024 * 1024
MAX_ENTRY_SIZE = 128 * 1024 * 1024
UML_FSTAB_PATH = "first_stage_ramdisk/system/etc/fstab.uml"
UML_FSTAB = b"""# Fixed GPT partitions exposed by the Darwin UML UBD device.
/dev/block/by-name/system /system erofs ro wait,first_stage_mount
/dev/block/by-name/system_ext /system_ext ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/product /product erofs ro wait,first_stage_mount
/dev/block/by-name/vendor /vendor ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/odm /odm ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/metadata /metadata ext4 nodev,noatime,nosuid wait,check,formattable,first_stage_mount
/dev/block/by-name/metadata /metadata ext4 nodev,noatime,nosuid wait,check,first_stage_mount
# Static non-A/B storage uses neither userdata checkpointing nor BootControl.
/dev/block/by-name/userdata /data ext4 nodev,noatime,nosuid,errors=panic latemount,wait,check,quota,formattable,keydirectory=/metadata/vold/metadata_encryption,fileencryption=aes-256-xts:aes-256-hctr2
/dev/block/by-name/userdata /data ext4 nodev,noatime,nosuid,errors=panic latemount,reservedsize=128M,wait,check,quota,keydirectory=/metadata/vold/metadata_encryption,fileencryption=aes-256-xts:aes-256-hctr2
"""
FORBIDDEN_PATHS = {
    "system/bin/modprobe",
    "system/bin/snapuserd",
    "system/bin/snapuserd_ramdisk",
    "system/etc/init/snapuserd.rc",
}
MODULE_METADATA = {
    "modules.alias",
    "modules.alias.bin",
    "modules.builtin",
    "modules.builtin.alias.bin",
    "modules.builtin.modinfo",
    "modules.dep",
    "modules.dep.bin",
    "modules.devname",
    "modules.load",
    "modules.options",
    "modules.order",
    "modules.softdep",
    "modules.symbols",
    "modules.symbols.bin",
}


@dataclass(frozen=True)
class Entry:
    mode: int
    size: int
    payload: bytes | None


def fail(message: str) -> None:
    raise SystemExit(f"Android initramfs verifier: {message}")


def align4(value: int) -> int:
    return (value + 3) & ~3


def safe_name(raw: bytes) -> str:
    try:
        name = raw.decode("utf-8", errors="strict")
    except UnicodeError as error:
        fail(f"archive path is not UTF-8: {error}")
    if not name or "\\" in name or "\0" in name:
        fail(f"unsafe archive path: {name!r}")
    path = PurePosixPath(name)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        fail(f"unsafe archive path: {name!r}")
    if str(path) != name:
        fail(f"non-normalized archive path: {name!r}")
    return name


def read_exact(source, count: int, label: str) -> bytes:
    payload = source.read(count)
    if len(payload) != count:
        fail(f"truncated {label}")
    return payload


def parse_newc(path: Path) -> dict[str, Entry]:
    size = path.stat().st_size
    if size == 0 or size > MAX_INITRAMFS_SIZE:
        fail(f"archive size is invalid: {size}")

    entries: dict[str, Entry] = {}
    with path.open("rb") as source:
        while True:
            header = read_exact(source, NEWC_HEADER_SIZE, "newc header")
            if header[:6] not in (b"070701", b"070702"):
                fail("archive is not an uncompressed newc initramfs")
            try:
                fields = [
                    int(header[offset : offset + 8], 16)
                    for offset in range(6, NEWC_HEADER_SIZE, 8)
                ]
            except ValueError as error:
                fail(f"malformed newc header: {error}")
            mode = fields[1]
            file_size = fields[6]
            name_size = fields[11]
            if file_size > MAX_ENTRY_SIZE or name_size < 2 or name_size > 4096:
                fail("newc entry has an invalid size")
            raw_name = read_exact(source, name_size, "newc path")
            if raw_name[-1:] != b"\0":
                fail("newc path is not NUL terminated")
            source.seek(align4(source.tell()))

            if raw_name[:-1] == b"TRAILER!!!":
                if file_size != 0:
                    fail("newc trailer has data")
                remainder = source.read()
                if any(remainder):
                    fail("newc archive has nonzero data after its trailer")
                break

            name = safe_name(raw_name[:-1])
            retain = name == UML_FSTAB_PATH or stat.S_ISLNK(mode)
            payload = read_exact(source, file_size, f"payload for {name}") if retain else None
            if not retain:
                source.seek(file_size, 1)
            source.seek(align4(source.tell()))
            entry = Entry(mode, file_size, payload)
            previous = entries.get(name)
            if previous is not None:
                duplicate_directory = (
                    stat.S_ISDIR(previous.mode)
                    and stat.S_ISDIR(entry.mode)
                    and stat.S_IMODE(previous.mode) == stat.S_IMODE(entry.mode)
                    and previous.size == entry.size == 0
                )
                if not duplicate_directory:
                    fail(f"ambiguous duplicate archive entry: {name}")
            entries[name] = entry
    return entries


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("initramfs", type=Path)
    args = parser.parse_args()
    if not args.initramfs.is_file() or args.initramfs.is_symlink():
        fail("input must be a regular non-symlink file")

    entries = parse_newc(args.initramfs)
    init = entries.get("init")
    if init is None or not stat.S_ISREG(init.mode) or not (init.mode & 0o111):
        fail("archive does not contain an executable regular /init")
    fstab = entries.get(UML_FSTAB_PATH)
    if fstab is None or not stat.S_ISREG(fstab.mode) or fstab.payload != UML_FSTAB:
        fail("archive does not contain the exact UML first-stage fstab")

    for name, entry in entries.items():
        path = PurePosixPath(name)
        lowered = path.name.lower()
        if name in FORBIDDEN_PATHS:
            fail(f"forbidden module or snapshot helper remains: {name}")
        if lowered.endswith(".ko") or ".ko." in lowered or lowered in MODULE_METADATA:
            fail(f"loadable-module payload remains: {name}")
        if entry.payload is not None and stat.S_ISLNK(entry.mode):
            target = entry.payload.decode("utf-8", errors="strict")
            if target in {"/system/bin/modprobe", "modprobe"}:
                fail(f"module-loader symlink remains: {name}")

    print(f"verified Android 17 UML initramfs: {len(entries)} entries, {args.initramfs.stat().st_size} bytes")


if __name__ == "__main__":
    main()
