#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import stat
from pathlib import Path, PurePosixPath

import android_image


MAX_INITRAMFS_SIZE = 256 * 1024 * 1024
UML_FSTAB_PATH = "first_stage_ramdisk/system/etc/fstab.uml"
UML_FSTAB = b"""# Fixed GPT partitions exposed by the Darwin UML UBD device.
/dev/block/by-name/system /system ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/system_ext /system_ext ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/product /product ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/vendor /vendor ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/odm /odm ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/metadata /metadata ext4 nodev,noatime,nosuid,discard wait,check,formattable,first_stage_mount
/dev/block/by-name/metadata /metadata ext4 nodev,noatime,nosuid,discard wait,check,first_stage_mount
# Static non-A/B storage uses neither userdata checkpointing nor BootControl.
/dev/block/by-name/userdata /data ext4 nodev,noatime,nosuid,discard,errors=panic latemount,wait,check,formattable
/dev/block/by-name/userdata /data ext4 nodev,noatime,nosuid,discard,errors=panic latemount,reservedsize=128M,wait,check
/dev/block/by-name/misc /misc emmc defaults defaults
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


def error(message: str) -> SystemExit:
    return SystemExit(f"Android initramfs verifier: {message}")


def fail(message: str) -> None:
    raise error(message)


def parse_newc(path: Path, offset: int, size: int) -> dict[str, android_image.NewcEntry]:
    if size == 0 or size > MAX_INITRAMFS_SIZE:
        fail(f"archive size is invalid: {size}")
    if offset < 0 or offset + size > path.stat().st_size:
        fail("archive range exceeds the input file")
    with path.open("rb") as source:
        return android_image.parse_newc(
            android_image.RangeReader(source, offset, size, error),
            lambda name, mode: name == UML_FSTAB_PATH or stat.S_ISLNK(mode),
            error,
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--structural",
        action="store_true",
        help="validate invariant archive safety without current fstab byte equality",
    )
    parser.add_argument("--offset", type=int)
    parser.add_argument("--size", type=int)
    parser.add_argument("initramfs", type=Path)
    args = parser.parse_args()
    if not args.initramfs.is_file() or args.initramfs.is_symlink():
        fail("input must be a regular non-symlink file")

    if (args.offset is None) != (args.size is None):
        fail("--offset and --size must be supplied together")
    offset = args.offset if args.offset is not None else 0
    size = args.size if args.size is not None else args.initramfs.stat().st_size
    entries = parse_newc(args.initramfs, offset, size)
    init = entries.get("init")
    if init is None or not stat.S_ISREG(init.mode) or not (init.mode & 0o111):
        fail("archive does not contain an executable regular /init")
    fstab = entries.get(UML_FSTAB_PATH)
    if fstab is None or not stat.S_ISREG(fstab.mode):
        fail("archive does not contain a regular UML first-stage fstab")
    if not args.structural and fstab.payload != UML_FSTAB:
        fail("archive does not contain the exact current UML first-stage fstab")

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

    print(f"verified Android 17 UML initramfs: {len(entries)} entries, {size} bytes")


if __name__ == "__main__":
    main()
