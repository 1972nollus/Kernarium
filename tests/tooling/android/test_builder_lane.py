#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import re
import subprocess
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
        android_root / "build/prepare-sources.sh",
        android_root / "build/preflight.sh",
        kernel_root / "README.md",
        kernel_root / "patches/metadata.env",
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
        "ANDROID17_MAJOR": "17",
        "ANDROID17_PAGE_SIZE": "16384",
    }
    expected_ack = {
        "UML_KERNEL_FAMILY": "android-6.18",
        "UML_KERNEL_UPSTREAM_TAG": "android17-6.18-2026-06_r35",
        "UML_KERNEL_UPSTREAM_COMMIT": "7942b4b6552dc2c5932ed2bd9a6f3981f6a04333",
        "UML_KERNEL_UPSTREAM_TREE": "24e0546bc6c78e8e829f0a3657c8ba7c68faabda",
        "UML_KERNEL_FINAL_TREE": "24e0546bc6c78e8e829f0a3657c8ba7c68faabda",
        "UML_KERNEL_PATCH_COUNT": "0",
        "UML_KERNEL_ABI_VERSION": "8",
        "UML_KERNEL_ABI_FINGERPRINT": "0x3871ad612337ea76",
        "UML_KERNEL_CLANG_VERSION": "r584948c",
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

    scripts = list((android_root / "build").glob("*.sh"))
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

    print("Android 17 builder lane ownership passed")


if __name__ == "__main__":
    main()
