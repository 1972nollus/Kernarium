#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
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


def verify_target_files_fixtures(verifier: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="android-target-files-") as temporary:
        root = Path(temporary)
        kernel = root / "vmlinux"
        kernel.write_bytes(synthetic_aarch64_elf())

        def write_fixture(path: Path, *, module: bool = False, abi32: bool = False,
                          alignment: int = 16384) -> None:
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("BOOT/kernel", kernel.read_bytes())
                archive.writestr("META/misc_info.txt", "")
                abilist32 = "armeabi-v7a" if abi32 else ""
                archive.writestr(
                    "SYSTEM/build.prop",
                    "ro.product.cpu.abi=arm64-v8a\n"
                    "ro.product.cpu.abilist=arm64-v8a\n"
                    f"ro.product.cpu.abilist32={abilist32}\n",
                )
                archive.writestr(
                    "SYSTEM/bin/android-fixture", synthetic_aarch64_elf(alignment)
                )
                if module:
                    archive.writestr("VENDOR/lib/modules/forbidden.ko", b"module")

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
            ("abi32", {"abi32": True}),
            ("alignment", {"alignment": 4096}),
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
        android_root / "tools/verify-aosp-patches.py",
        android_root / "tools/verify-target-files.py",
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
    build_kernel = (android_root / "build/build-kernel.sh").read_text()
    kernel_config = (kernel_root / "config/android.fragment").read_text()
    if "worktree add --detach" not in apply_kernel or "git -C \"$KERNEL_SOURCE\" am" not in apply_kernel:
        fail("ACK materializer does not replay generated topics in a worktree")
    if "patches/projects.tsv" not in apply_aosp or 'git -C "$checkout" am' not in apply_aosp:
        fail("AOSP materializer does not replay the routed per-project series")
    if "$ANDROID_ROOT/support" in apply_aosp or "cp " in apply_aosp:
        fail("AOSP materializer stages an overlay instead of applying project patches")
    for token in (
        "command -v zip",
        "--kernel-elf",
        "verify-kernel-elf.py",
        "verify-target-files.py",
        "target_files_intermediates",
        'install -m 0755 -- "$KERNEL_ELF" "$PRODUCT_OUT/kernel"',
        "SOONG_NINJA=ninja",
        "SOONG_INCREMENTAL_ANALYSIS=false",
        "TARGET_2ND_ARCH",
        "TARGET_CHECK_PREBUILT_MAX_PAGE_SIZE",
        "PRODUCT_BUILD_SYSTEM_DLKM_IMAGE",
        "PRODUCT_BUILD_VENDOR_DLKM_IMAGE",
        "PRODUCT_BUILD_ODM_DLKM_IMAGE",
        "BOARD_VENDOR_RAMDISK_KERNEL_MODULES",
        "BOARD_SYSTEM_KERNEL_MODULES",
        "target-files-package",
    ):
        if token not in build_aosp:
            fail(f"AOSP builder is missing fixed product policy: {token}")
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

    print("Android 17 builder lane ownership passed")


if __name__ == "__main__":
    main()
