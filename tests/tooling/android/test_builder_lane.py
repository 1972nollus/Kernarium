#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import re
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"android builder lane fixture: {message}")


def parse_env(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    assignment = re.compile(r"^([A-Z][A-Z0-9_]*)=(?:'([^']*)'|([0-9]+))$")
    for line_number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = assignment.fullmatch(line)
        if match is None:
            fail(f"{path}:{line_number}: malformed metadata assignment")
        key, quoted, numeric = match.groups()
        if key in values:
            fail(f"{path}:{line_number}: duplicate metadata key {key}")
        values[key] = quoted if quoted is not None else numeric
    return values


def synthetic_aarch64_elf(alignment: int = 16384) -> bytes:
    payload = bytearray(64 + 56)
    payload[:4] = b"\x7fELF"
    payload[4] = 2
    payload[5] = 1
    payload[6] = 1
    struct.pack_into("<HHI", payload, 16, 3, 183, 1)
    struct.pack_into("<Q", payload, 32, 64)
    struct.pack_into("<HHH", payload, 52, 64, 56, 1)
    struct.pack_into(
        "<IIQQQQQQ",
        payload,
        64,
        1,
        5,
        0,
        0,
        0,
        len(payload),
        len(payload),
        alignment,
    )
    return bytes(payload)


UML_FSTAB = """# Fixed GPT partitions exposed by the Darwin UML UBD device.
/dev/block/by-name/system /system erofs ro wait,first_stage_mount
/dev/block/by-name/system_ext /system_ext ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/product /product erofs ro wait,first_stage_mount
/dev/block/by-name/vendor /vendor ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/odm /odm ext4 noatime,ro,errors=panic wait,first_stage_mount
/dev/block/by-name/metadata /metadata ext4 nodev,noatime,nosuid wait,check,formattable,first_stage_mount
/dev/block/by-name/metadata /metadata ext4 nodev,noatime,nosuid wait,check,first_stage_mount
# Static non-A/B storage uses neither userdata checkpointing nor BootControl.
/dev/block/by-name/userdata /data ext4 nodev,noatime,nosuid,errors=panic latemount,wait,check,formattable,keydirectory=/metadata/vold/metadata_encryption,fileencryption=aes-256-xts:aes-256-hctr2
/dev/block/by-name/userdata /data ext4 nodev,noatime,nosuid,errors=panic latemount,reservedsize=128M,wait,check,keydirectory=/metadata/vold/metadata_encryption,fileencryption=aes-256-xts:aes-256-hctr2
"""


def verify_target_files_fixtures(verifier: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="android-target-files-") as temporary:
        root = Path(temporary)
        kernel = root / "vmlinux"
        kernel.write_bytes(synthetic_aarch64_elf())
        kernel_config = root / ".config"
        kernel_config.write_text(
            "CONFIG_ARM64_16K_PAGES=y\n# CONFIG_MODULES is not set\n"
        )
        kernel_release = root / "include/config/kernel.release"
        kernel_release.parent.mkdir(parents=True)
        kernel_release.write_text("6.18.21-uml-fixture\n")

        def write_fixture(path: Path, *, module: bool = False,
                          module_loader: bool = False, dlkm_image: bool = False,
                          dynamic_image: bool = False, ota_tool: bool = False,
                          virtual_ab: bool = False, abi32: bool = False,
                          wrong_fstab: bool = False, stock_fstab: bool = False,
                          seriallogging: bool = False,
                          boot_control: bool = False,
                          wrong_kernel_config: bool = False,
                          wrong_kernel_release: bool = False,
                          missing_memfd_policy: bool = False,
                          alignment: int = 16384) -> None:
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("BOOT/kernel", kernel.read_bytes())
                archive.writestr("INIT_BOOT/RAMDISK/init", b"init")
                archive.writestr("META/init_boot_filesystem_config.txt", "")
                archive.writestr(
                    "META/kernel_configs.txt",
                    "CONFIG_ARM64_16K_PAGES=n\n"
                    if wrong_kernel_config
                    else kernel_config.read_bytes(),
                )
                archive.writestr(
                    "META/kernel_version.txt",
                    "6.18.21-wrong\n"
                    if wrong_kernel_release
                    else kernel_release.read_bytes(),
                )
                archive.writestr("META/misc_info.txt", "")
                archive.writestr("META/ramdisk_node_list", "")
                for image in (
                    "odm.img",
                    "product.img",
                    "system.img",
                    "system_ext.img",
                    "userdata.img",
                    "vendor.img",
                ):
                    archive.writestr(f"IMAGES/{image}", b"image")
                archive.writestr("ROOT/system_dlkm/", b"")
                archive.writestr("SYSTEM/system_dlkm/etc/build.prop", "")
                abilist32 = "armeabi-v7a" if abi32 else ""
                virtual_ab_property = "ro.virtual_ab.enabled=true\n" if virtual_ab else ""
                archive.writestr(
                    "SYSTEM/build.prop",
                    "ro.product.cpu.abi=arm64-v8a\n"
                    "ro.product.cpu.abilist=arm64-v8a\n"
                    f"ro.product.cpu.abilist32={abilist32}\n"
                    f"{virtual_ab_property}",
                )
                archive.writestr(
                    "SYSTEM/bin/android-fixture", synthetic_aarch64_elf(alignment)
                )
                fstab = UML_FSTAB.replace("/system erofs", "/system ext4", 1) if wrong_fstab else UML_FSTAB
                archive.writestr("VENDOR/etc/fstab.uml", fstab)
                archive.writestr(
                    "VENDOR_BOOT/RAMDISK/first_stage_ramdisk/system/etc/fstab.uml",
                    fstab,
                )
                archive.writestr(
                    "VENDOR/etc/init/init.cutf_cvm.rc",
                    "on fs\n    mount_all --early\n"
                    "on late-fs\n    mount_all --late\n"
                    "on boot\n    setprop ro.hardware.audio.primary goldfish\n",
                )
                archive.writestr(
                    "VENDOR/etc/selinux/vendor_sepolicy.cil",
                    "(policycap open_perms)\n"
                    if missing_memfd_policy
                    else "(policycap memfd_class)\n",
                )
                archive.writestr("VENDOR/etc/ueventd.rc", "/dev/ttyS0 0660 root shell\n")
                archive.writestr(
                    "VENDOR/etc/vintf/compatibility_matrix.xml",
                    "<compatibility-matrix/>\n",
                )
                archive.writestr("VENDOR/etc/vintf/manifest.xml", "<manifest/>\n")
                if module:
                    archive.writestr("VENDOR/lib/modules/forbidden.ko", b"module")
                if module_loader:
                    archive.writestr("VENDOR/bin/dlkm_loader", b"loader")
                if dlkm_image:
                    archive.writestr("SYSTEM_DLKM/etc/build.prop", "")
                if dynamic_image:
                    archive.writestr("IMAGES/super_empty.img", b"dynamic")
                if ota_tool:
                    archive.writestr("OTA/bin/updater", b"updater")
                if stock_fstab:
                    archive.writestr("VENDOR/etc/fstab.cf.ext4.hctr2", b"stock")
                if seriallogging:
                    archive.writestr("VENDOR/etc/init/seriallogging.rc", b"stock")
                if boot_control:
                    archive.writestr(
                        "VENDOR/apex/com.android.hardware.boot.apex", b"apex"
                    )

        good = root / "good.zip"
        write_fixture(good)
        subprocess.run(
            [
                sys.executable,
                "-B",
                str(verifier),
                "--target-files",
                str(good),
                "--kernel-elf",
                str(kernel),
            ],
            check=True,
        )

        for label, options in (
            ("module", {"module": True}),
            ("module-loader", {"module_loader": True}),
            ("dlkm-image", {"dlkm_image": True}),
            ("dynamic-image", {"dynamic_image": True}),
            ("ota-tool", {"ota_tool": True}),
            ("virtual-ab", {"virtual_ab": True}),
            ("abi32", {"abi32": True}),
            ("alignment", {"alignment": 4096}),
            ("wrong-fstab", {"wrong_fstab": True}),
            ("stock-fstab", {"stock_fstab": True}),
            ("seriallogging", {"seriallogging": True}),
            ("boot-control", {"boot_control": True}),
            ("kernel-config", {"wrong_kernel_config": True}),
            ("kernel-release", {"wrong_kernel_release": True}),
            ("memfd-policy", {"missing_memfd_policy": True}),
        ):
            fixture = root / f"bad-{label}.zip"
            write_fixture(fixture, **options)
            result = subprocess.run(
                [
                    sys.executable,
                    "-B",
                    str(verifier),
                    "--target-files",
                    str(fixture),
                    "--kernel-elf",
                    str(kernel),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            if result.returncode == 0:
                fail(f"target-files verifier accepted the {label} fixture")


def newc_entry(name: str, mode: int, payload: bytes, inode: int) -> bytes:
    fields = (
        inode,
        mode,
        0,
        0,
        1,
        0,
        len(payload),
        0,
        0,
        0,
        0,
        len(name.encode()) + 1,
        0,
    )
    result = bytearray(b"070701" + b"".join(f"{value:08x}".encode() for value in fields))
    result.extend(name.encode() + b"\0")
    result.extend(b"\0" * (-len(result) % 4))
    result.extend(payload)
    result.extend(b"\0" * (-len(result) % 4))
    return bytes(result)


def synthetic_initramfs(*, wrong_fstab: bool = False, module_loader: bool = False,
                        duplicate_file: bool = False) -> bytes:
    fstab = UML_FSTAB.replace("/system erofs", "/system ext4", 1) if wrong_fstab else UML_FSTAB
    entries = [
        newc_entry("dev", 0o40755, b"", 1),
        newc_entry("dev", 0o40755, b"", 2),
        newc_entry("init", 0o100750, b"init", 3),
        newc_entry(
            "first_stage_ramdisk/system/etc/fstab.uml",
            0o100644,
            fstab.encode(),
            4,
        ),
    ]
    if module_loader:
        entries.append(newc_entry("system/bin/modprobe", 0o120777, b"toybox", 5))
    if duplicate_file:
        entries.append(newc_entry("init", 0o100750, b"init", 6))
    entries.append(newc_entry("TRAILER!!!", 0, b"", len(entries) + 1))
    return b"".join(entries)


def verify_initramfs_fixtures(verifier: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="android-initramfs-") as temporary:
        root = Path(temporary)
        good = root / "ramdisk.img"
        good.write_bytes(synthetic_initramfs())
        subprocess.run([sys.executable, "-B", str(verifier), str(good)], check=True)
        for label, options in (
            ("wrong-fstab", {"wrong_fstab": True}),
            ("module-loader", {"module_loader": True}),
            ("duplicate-file", {"duplicate_file": True}),
        ):
            fixture = root / f"bad-{label}.img"
            fixture.write_bytes(synthetic_initramfs(**options))
            result = subprocess.run(
                [sys.executable, "-B", str(verifier), str(fixture)],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            if result.returncode == 0:
                fail(f"initramfs verifier accepted the {label} fixture")


def synthetic_sparse_image() -> bytes:
    block_size = 4096
    raw = b"A" * block_size
    return b"".join(
        (
            struct.pack(
                "<IHHHHIIII",
                0xED26FF3A,
                1,
                0,
                28,
                12,
                block_size,
                3,
                3,
                0,
            ),
            struct.pack("<HHII", 0xCAC1, 0, 1, 12 + block_size),
            raw,
            struct.pack("<HHII", 0xCAC2, 0, 1, 16),
            b"\x12\x34\x56\x78",
            struct.pack("<HHII", 0xCAC3, 0, 1, 12),
        )
    )


def verify_ubd_disk_fixtures(tool: Path, production_layout: Path) -> None:
    subprocess.run(
        [
            sys.executable,
            "-B",
            str(tool),
            "validate",
            "--layout",
            str(production_layout),
        ],
        check=True,
    )
    layout = json.loads(production_layout.read_text())
    names = [partition["name"] for partition in layout["partitions"]]
    if names != [
        "system",
        "system_ext",
        "product",
        "vendor",
        "odm",
        "metadata",
        "userdata",
    ]:
        fail("production UBD layout has the wrong fixed partition order")
    if "super" in names or any("dlkm" in name for name in names):
        fail("production UBD layout contains a dynamic or DLKM partition")
    if layout["sector_size"] != 512 or layout["alignment_bytes"] != 1024 * 1024:
        fail("production UBD layout has the wrong sector geometry")
    for partition in layout["partitions"]:
        if partition["start_bytes"] % layout["alignment_bytes"]:
            fail(f"production UBD partition is misaligned: {partition['name']}")
        if partition["size_bytes"] % layout["alignment_bytes"]:
            fail(f"production UBD partition has a partial alignment unit: {partition['name']}")

    with tempfile.TemporaryDirectory(prefix="android-ubd-disk-") as temporary:
        root = Path(temporary)
        images = root / "images"
        images.mkdir()
        (images / "system.img").write_bytes(b"raw-system")
        userdata_payload = synthetic_sparse_image()
        (images / "userdata.img").write_bytes(userdata_payload)
        fixture_layout = root / "layout.json"
        fixture_layout.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "sector_size": 512,
                    "alignment_bytes": 1024 * 1024,
                    "disk_size_bytes": 16 * 1024 * 1024,
                    "partition_entry_count": 128,
                    "disk_guid": "253a21bd-933f-5273-8cd3-586ec7288d68",
                    "partitions": [
                        {
                            "name": "system",
                            "start_bytes": 1024 * 1024,
                            "size_bytes": 2 * 1024 * 1024,
                            "image": "system.img",
                            "type_guid": "0fc63daf-8483-4772-8e79-3d69d8477de4",
                            "unique_guid": "e2c6424a-9fda-551e-b5d2-c1ba3b29ae33",
                            "writable": False,
                        },
                        {
                            "name": "userdata",
                            "start_bytes": 3 * 1024 * 1024,
                            "size_bytes": 4 * 1024 * 1024,
                            "image": "userdata.img",
                            "type_guid": "0fc63daf-8483-4772-8e79-3d69d8477de4",
                            "unique_guid": "f07a1ac4-5c95-5c37-86dd-70eaa487a193",
                            "writable": True,
                        },
                    ],
                },
                indent=2,
            )
            + "\n"
        )
        disk = root / "android.img"
        subprocess.run(
            [
                sys.executable,
                "-B",
                str(tool),
                "assemble",
                "--layout",
                str(fixture_layout),
                "--images-dir",
                str(images),
                "--output",
                str(disk),
            ],
            check=True,
        )
        subprocess.run(
            [
                sys.executable,
                "-B",
                str(tool),
                "verify",
                "--layout",
                str(fixture_layout),
                "--disk",
                str(disk),
            ],
            check=True,
        )
        if disk.stat().st_size != 16 * 1024 * 1024:
            fail("assembled UBD fixture has the wrong logical size")
        with disk.open("rb") as image:
            image.seek(1024 * 1024)
            if image.read(len(b"raw-system")) != b"raw-system":
                fail("raw partition payload was not staged at its fixed offset")
            image.seek(3 * 1024 * 1024)
            if image.read(4096) != b"A" * 4096:
                fail("Android sparse RAW chunk was not expanded")
            if image.read(8) != b"\x12\x34\x56\x78" * 2:
                fail("Android sparse FILL chunk was not expanded")
            image.seek(3 * 1024 * 1024 + 2 * 4096)
            if image.read(32) != b"\0" * 32:
                fail("Android sparse DONT_CARE chunk is not a disk hole")

        existing = subprocess.run(
            [
                sys.executable,
                "-B",
                str(tool),
                "assemble",
                "--layout",
                str(fixture_layout),
                "--images-dir",
                str(images),
                "--output",
                str(disk),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if existing.returncode == 0:
            fail("UBD assembler replaced an existing disk without permission")

        (images / "userdata.img").unlink()
        missing = subprocess.run(
            [
                sys.executable,
                "-B",
                str(tool),
                "assemble",
                "--layout",
                str(fixture_layout),
                "--images-dir",
                str(images),
                "--output",
                str(root / "missing.img"),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if missing.returncode == 0:
            fail("UBD assembler silently accepted a missing writable image")

        blank = root / "blank.img"
        subprocess.run(
            [
                sys.executable,
                "-B",
                str(tool),
                "assemble",
                "--layout",
                str(fixture_layout),
                "--images-dir",
                str(images),
                "--blank-partition",
                "userdata",
                "--output",
                str(blank),
            ],
            check=True,
        )
        with blank.open("rb") as image:
            image.seek(3 * 1024 * 1024)
            if image.read(4096) != b"\0" * 4096:
                fail("explicit blank UBD partition is not zero initialized")

        for label, partition in (("immutable", "system"), ("unknown", "missing")):
            rejected = subprocess.run(
                [
                    sys.executable,
                    "-B",
                    str(tool),
                    "assemble",
                    "--layout",
                    str(fixture_layout),
                    "--images-dir",
                    str(images),
                    "--blank-partition",
                    partition,
                    "--output",
                    str(root / f"bad-blank-{label}.img"),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            if rejected.returncode == 0:
                fail(f"UBD assembler accepted an {label} blank partition")
        (images / "userdata.img").write_bytes(userdata_payload)

        with disk.open("r+b") as image:
            image.seek(512 + 20)
            byte = image.read(1)
            image.seek(512 + 20)
            image.write(bytes((byte[0] ^ 0x80,)))
        corrupted = subprocess.run(
            [
                sys.executable,
                "-B",
                str(tool),
                "verify",
                "--layout",
                str(fixture_layout),
                "--disk",
                str(disk),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if corrupted.returncode == 0:
            fail("UBD verifier accepted a corrupted primary GPT")

        (images / "system.img").write_bytes(b"X" * (2 * 1024 * 1024 + 1))
        oversized = subprocess.run(
            [
                sys.executable,
                "-B",
                str(tool),
                "assemble",
                "--layout",
                str(fixture_layout),
                "--images-dir",
                str(images),
                "--output",
                str(root / "oversized.img"),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if oversized.returncode == 0:
            fail("UBD assembler accepted a partition payload that exceeds its capacity")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    root = args.source_root.resolve()

    android_root = root / "android/aosp-17"
    kernel_root = root / "kernel/android-6.18"
    required = [
        root / "android/README.md",
        android_root / "README.md",
        android_root / "metadata/aosp.env",
        android_root / "metadata/base-manifest.xml",
        android_root / "build/prepare-sources.sh",
        android_root / "build/preflight.sh",
        android_root / "build/apply-aosp-patches.sh",
        android_root / "build/build-aosp.sh",
        android_root / "build/assemble-image.sh",
        android_root / "build/assemble-ramdisk.sh",
        android_root / "build/apply-kernel-patches.sh",
        android_root / "build/build-kernel.sh",
        android_root / "build/verify-kernel-elf.py",
        android_root / "build/toolchain/aarch64-linux-clang.sh",
        android_root / "build/toolchain/linux-host-clang.sh",
        android_root / "build/toolchain/linux-host-compat.h",
        android_root / "support/verification/required-kernel-builtins.txt",
        android_root / "support/verification/forbidden-kernel-options.txt",
        android_root / "patches/projects.tsv",
        android_root / "patches/projects/p0001/metadata.env",
        android_root / "patches/projects/p0001/series/manifest.tsv",
        android_root / "patches/projects/p0002/metadata.env",
        android_root / "patches/projects/p0002/series/manifest.tsv",
        android_root / "patches/projects/p0003/metadata.env",
        android_root / "patches/projects/p0003/series/manifest.tsv",
        android_root / "tools/verify-aosp-patches.py",
        android_root / "tools/regenerate-aosp-patches.py",
        android_root / "tools/verify-target-files.py",
        android_root / "tools/verify-initramfs.py",
        android_root / "tools/ubd-disk.py",
        android_root / "support/boot/kernel-command-line.txt",
        android_root / "support/image/partition-layout.json",
        kernel_root / "README.md",
        kernel_root / "config/android.fragment",
        kernel_root / "patches/metadata.env",
        kernel_root / "patches/topic-path-overlaps.tsv",
        kernel_root / "patches/series/manifest.tsv",
    ]
    for path in required:
        if not path.is_file():
            fail(f"required owner is missing: {path.relative_to(root)}")

    aosp = parse_env(android_root / "metadata/aosp.env")
    ack = parse_env(kernel_root / "patches/metadata.env")
    expected_aosp = {
        "ANDROID17_MANIFEST_TAG": "android-17.0.0_r1",
        "ANDROID17_MANIFEST_COMMIT": "5bc9a7ce1cd78dd53613bbfd0ebf506e1e4adb0f",
        "ANDROID17_MANIFEST_TREE": "1541b7154f1532032baf7c73f222256cc29e8cfb",
        "ANDROID17_BASE_MANIFEST_SHA256": "3aa2cfa6bf79012561b33755177e094cddd9f6488ba2f7fca583f12cbba698c1",
        "ANDROID17_MAJOR": "17",
        "ANDROID17_PRODUCT": "aosp_uml_arm64_pgagnostic",
        "ANDROID17_PAGE_SIZE": "16384",
        "ANDROID17_RELEASE_CONFIG": "trunk_staging",
    }
    expected_ack = {
        "UML_KERNEL_FAMILY": "android-6.18",
        "UML_KERNEL_UPSTREAM_TAG": "android17-6.18-2026-06_r35",
        "UML_KERNEL_UPSTREAM_COMMIT": "7942b4b6552dc2c5932ed2bd9a6f3981f6a04333",
        "UML_KERNEL_UPSTREAM_TREE": "24e0546bc6c78e8e829f0a3657c8ba7c68faabda",
        "UML_KERNEL_FINAL_TREE": "ef82c4396c776d300101fbb951509d1e3be36b4d",
        "UML_KERNEL_PATCH_COUNT": "23",
        "UML_KERNEL_ABI_VERSION": "8",
        "UML_KERNEL_ABI_FINGERPRINT": "0x3871ad612337ea76",
        "UML_KERNEL_CLANG_VERSION": "r584948c",
        "UML_KERNEL_RUST_VERSION": "1.91.1.p3",
        "UML_KERNEL_BINDGEN_VERSION": "0.72.1",
        "UML_KERNEL_HOST_GLIBC_TOOLCHAIN": "x86_64-linux-glibc2.17-4.8",
    }
    for key, value in expected_aosp.items():
        if aosp.get(key) != value:
            fail(f"AOSP metadata {key} is not pinned to {value}")
    for key, value in expected_ack.items():
        if ack.get(key) != value:
            fail(f"ACK metadata {key} is not pinned to {value}")

    verifier = root / "kernel/common/tooling/verify-series-policy.py"
    subprocess.run(
        [str(verifier), "--line-root", str(kernel_root), "manifest"], check=True
    )
    subprocess.run(
        [
            str(android_root / "tools/verify-aosp-patches.py"),
            "--android-root",
            str(android_root),
        ],
        check=True,
    )
    verify_target_files_fixtures(android_root / "tools/verify-target-files.py")
    verify_initramfs_fixtures(android_root / "tools/verify-initramfs.py")
    verify_ubd_disk_fixtures(
        android_root / "tools/ubd-disk.py",
        android_root / "support/image/partition-layout.json",
    )

    scripts = list((android_root / "build").rglob("*.sh"))
    scripts += list((android_root / "tools").rglob("*.py"))
    forbidden_commands = re.compile(
        r"(^|[;&|()\s])(ssh|scp|sftp|rsync|rsh)([;&|()\s]|$)", re.MULTILINE
    )
    forbidden_endpoint = re.compile(
        r"(BUILDER_(HOST|USER|PORT)|SSH_|CREDENTIAL|PRIVATE_KEY|REMOTE_URI)"
    )
    for script in scripts:
        text = script.read_text()
        if forbidden_commands.search(text):
            fail(f"tracked producer contains a transport command: {script}")
        if forbidden_endpoint.search(text):
            fail(f"tracked producer contains endpoint or credential state: {script}")
        if "/home/build/" in text:
            fail(f"tracked producer stores a builder-specific path: {script}")

    tracked_policy_text = "\n".join(
        path.read_text(errors="surrogateescape")
        for path in (
            android_root / "README.md",
            android_root / "metadata/aosp.env",
            kernel_root / "README.md",
            kernel_root / "config/android.fragment",
            kernel_root / "patches/metadata.env",
        )
    )
    for token in ("BUILDER_ALIAS", "/home/build/", "BUILDER_HOST", "PRIVATE_KEY"):
        if token in tracked_policy_text:
            fail(f"tracked Android policy stores operator state: {token}")

    prepare = (android_root / "build/prepare-sources.sh").read_text()
    preflight = (android_root / "build/preflight.sh").read_text()
    if "--aosp-source" not in prepare or "--ack-workspace" not in prepare:
        fail("source preparation does not require explicit local workspaces")
    for token in (
        "builder host must be Linux",
        "builder host must be x86_64",
        "ANDROID17_PAGE_SIZE\" -eq 16384",
        "UML_KERNEL_FAMILY\" = android-6.18",
        "ACK common",
    ):
        if token not in preflight:
            fail(f"builder preflight is missing policy: {token}")
    for script_name, text in (("prepare", prepare), ("preflight", preflight)):
        if "repo status" in text:
            fail(f"{script_name} mistakes Repo's clean status prose for a delta")
        if "repo forall" not in text or "git status --porcelain" not in text:
            fail(f"{script_name} does not inspect each project porcelain status")

    apply_kernel = (android_root / "build/apply-kernel-patches.sh").read_text()
    apply_aosp = (android_root / "build/apply-aosp-patches.sh").read_text()
    build_aosp = (android_root / "build/build-aosp.sh").read_text()
    assemble_image = (android_root / "build/assemble-image.sh").read_text()
    assemble_ramdisk = (android_root / "build/assemble-ramdisk.sh").read_text()
    publish_release = (android_root / "tools/publish-release.py").read_text()
    build_kernel = (android_root / "build/build-kernel.sh").read_text()
    target_files_verifier = (
        android_root / "tools/verify-target-files.py"
    ).read_text()
    kernel_config = (kernel_root / "config/android.fragment").read_text()
    build_make_patch = next(
        (android_root / "patches/projects/p0002/series").glob("*.patch")
    ).read_text()
    connectivity_patch = next(
        (android_root / "patches/projects/p0003/series").glob("*.patch")
    ).read_text()
    cuttlefish_patches = "\n".join(
        path.read_text()
        for path in sorted(
            (android_root / "patches/projects/p0001/series").glob("*.patch")
        )
    )
    if "worktree add --detach" not in apply_kernel or "git -C \"$KERNEL_SOURCE\" am" not in apply_kernel:
        fail("ACK materializer does not replay generated topics in a worktree")
    if "patches/projects.tsv" not in apply_aosp or 'git -C "$checkout" am' not in apply_aosp:
        fail("AOSP materializer does not replay the routed per-project series")
    if "$ANDROID_ROOT/support" in apply_aosp or "cp " in apply_aosp:
        fail("AOSP materializer stages an overlay instead of applying project patches")
    if "TARGET_SKIP_OTA_PACKAGE" not in build_make_patch or "built_ota_tools" not in build_make_patch:
        fail("AOSP build route does not remove the skipped-OTA updater dependency")
    for token in (
        "platform/packages/modules/Connectivity",
        "ro.boot.uml_bpf_interpreter",
        "api_level_full >= NETBPFLOAD_25Q2_VER && !umlInterpreter",
        "UML kernel has no eBPF JIT; using its built-in interpreter.",
        "isAtLeastKernelVersion(6, 6, 118) && !umlInterpreter",
        "UML BPF interpreter cannot execute callbacks; using inline BPF objects.",
    ):
        if token not in connectivity_patch and token not in (
            android_root / "patches/projects.tsv"
        ).read_text():
            fail(f"Connectivity patch route is missing UML BPF policy: {token}")
    for token in (
        "UML_ANDROID_DISABLE_PROJECT_QUOTAS := true",
        "ifeq ($(UML_ANDROID_DISABLE_PROJECT_QUOTAS),true)",
        "external_storage.casefold.enabled=1",
        "external_storage.sdcardfs.enabled=0",
        "latemount,wait,check,formattable,keydirectory=",
    ):
        if token not in cuttlefish_patches:
            fail(f"Cuttlefish patch route is missing userdata quota policy: {token}")
    for token in (
        "verify-target-files.py",
        "ubd-disk.py",
        "--blank-partition metadata",
        'TARGET_FILES_DIR=${TARGET_FILES%.zip}',
    ):
        if token not in assemble_image:
            fail(f"Android disk assembler is missing fixed policy: {token}")
    for token in (
        "verify-target-files.py",
        "verify-initramfs.py",
        "--mkbootfs",
        'TARGET_FILES_DIR=${TARGET_FILES%.zip}',
        "first_stage_ramdisk/system/etc/fstab.uml",
        "system/bin/modprobe",
        "system/bin/snapuserd",
    ):
        if token not in assemble_ramdisk:
            fail(f"Android ramdisk assembler is missing fixed policy: {token}")
    for token in (
        "command -v zip",
        "--kernel-elf",
        "verify-kernel-elf.py",
        "verify-target-files.py",
        "check_target_files_vintf",
        "target_files_intermediates",
        'install -m 0755 -- "$KERNEL_ELF" "$PRODUCT_OUT/kernel"',
        "SOONG_NINJA=ninja",
        "SOONG_INCREMENTAL_ANALYSIS=false",
        "PARTITION_LAYOUT",
        "partition_size_from_layout",
        "TARGET_2ND_ARCH",
        "TARGET_CHECK_PREBUILT_MAX_PAGE_SIZE",
        "PRODUCT_USE_DYNAMIC_PARTITIONS",
        "PRODUCT_QUOTA_PROJID",
        "external_storage.projid.enabled=*",
        "PRODUCT_BUILD_SUPER_PARTITION",
        "PRODUCT_BUILD_SYSTEM_OTHER_IMAGE",
        "PRODUCT_VIRTUAL_AB_OTA",
        "AB_OTA_UPDATER",
        "BOARD_AVB_ENABLE",
        "BOARD_SUPER_PARTITION_SIZE",
        "AB_OTA_POSTINSTALL_CONFIG",
        "BOARD_SYSTEMIMAGE_PARTITION_SIZE",
        "BOARD_USERDATAIMAGE_PARTITION_SIZE",
        "ro.virtual_ab.*",
        "PRODUCT_BUILD_SYSTEM_DLKM_IMAGE",
        "PRODUCT_BUILD_VENDOR_DLKM_IMAGE",
        "PRODUCT_BUILD_ODM_DLKM_IMAGE",
        "BOARD_USES_SYSTEM_DLKMIMAGE",
        "BOARD_VENDOR_DLKMIMAGE_FILE_SYSTEM_TYPE",
        "AB_OTA_PARTITIONS",
        "BOARD_VENDOR_RAMDISK_KERNEL_MODULES",
        "BOARD_SYSTEM_KERNEL_MODULES",
        "TRUSTY_SYSTEM_VM",
        "com.android.hardware.gatekeeper.nonsecure",
        "com.android.hardware.keymint.rust_nonsecure",
        "dlkm_loader",
        "checkpoint_gc",
        "com.android.hardware.boot",
        "android.hardware.boot-service.default_recovery",
        "update_engine_sideload",
        "trusty_security_vm.elf",
        "target-files-package",
        "device_google_cuttlefish_uml_config_ueventd_rc",
        "fstab.uml.vendor_ramdisk",
    ):
        if token not in build_aosp:
            fail(f"AOSP builder is missing fixed product policy: {token}")
    for token in (
        "META/kernel_configs.txt",
        "META/kernel_version.txt",
        "policycap memfd_class",
        "VENDOR/etc/vintf/manifest.xml",
        "VENDOR/etc/vintf/compatibility_matrix.xml",
        "VENDOR/apex/com.android.hardware.boot.apex",
    ):
        if token not in target_files_verifier:
            fail(f"target-files verifier is missing fixed policy: {token}")
    for token in (
        "verify-aosp-patches.py",
        "verify-series-policy.py",
        "verify-kernel-elf.py",
        "verify-target-files.py",
        "verify-initramfs.py",
        "ubd-disk.py",
        "check_no_module_output",
        "verify_kernel_config",
        'parser.add_argument("--clang"',
        'parser.add_argument("--linker"',
        "enforce_tracked_policy=True",
        "release-manifest.json",
        "android-17-uml-arm64.img.zst",
        "repo",
        "manifest",
        '"-r"',
        "os.replace",
    ):
        if token not in publish_release:
            fail(f"Android release publisher is missing fixed policy: {token}")

    command_line = (android_root / "support/boot/kernel-command-line.txt").read_text()
    for token in (
        "init=/init",
        "console=tty0",
        "console=ttyS0",
        "androidboot.fstab_suffix=uml",
        "androidboot.boot_devices=uml-blkdev.0",
        "androidboot.selinux=enforcing",
        "androidboot.serialconsole=1",
        "androidboot.uml_bpf_interpreter=1",
    ):
        if token not in command_line.split():
            fail(f"Android kernel command line is missing fixed token: {token}")
    if "ubd0=" in command_line or "/home/" in command_line:
        fail("Android kernel command line stores a machine-specific disk path")
    for token in (
        "CONFIG_MODULES=n",
        "CONFIG_[A-Za-z0-9_]+=m$",
        "-name '*.ko'",
        "--page-size 16k",
        "verify-kernel-elf.py",
    ):
        if token not in build_kernel:
            fail(f"Android kernel build is missing fixed built-in policy: {token}")
    if "# CONFIG_MODULES is not set" not in kernel_config:
        fail("Android kernel fragment does not disable loadable modules")
    if "CONFIG_MSEAL_SYSTEM_MAPPINGS=y" not in kernel_config:
        fail("Android kernel fragment does not enable system mapping sealing")
    for symbol in (
        "CONFIG_BPF=y",
        "CONFIG_BPF_SYSCALL=y",
        "CONFIG_BLK_INLINE_ENCRYPTION=y",
        "CONFIG_BLK_INLINE_ENCRYPTION_FALLBACK=y",
        "CONFIG_CGROUP_BPF=y",
        "CONFIG_CRYPTO_HCTR2=y",
        "CONFIG_FS_ENCRYPTION_INLINE_CRYPT=y",
        "CONFIG_DM_DEFAULT_KEY=y",
    ):
        if symbol not in kernel_config:
            fail(f"Android kernel fragment does not build in {symbol}")

    print("Android 17 builder lane ownership passed")


if __name__ == "__main__":
    main()
