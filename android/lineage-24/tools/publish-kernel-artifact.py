#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Publish the already-built Android ACK kernel independently of the rootfs."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import platform
import re
import sys
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"Android ACK kernel artifact publisher: {message}")


def absolute_directory(path: Path, label: str) -> Path:
    if not path.is_absolute():
        fail(f"{label} must be an absolute builder-local path")
    resolved = path.resolve()
    if not resolved.is_dir() or resolved.is_symlink():
        fail(f"{label} is not a regular directory: {resolved}")
    return resolved


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--release-id", required=True)
    parser.add_argument(
        "--objcopy",
        type=Path,
        required=True,
        help="the pinned ACK llvm-objcopy that strips DWARF from vmlinux",
    )
    args = parser.parse_args()

    if platform.system() != "Linux" or platform.machine() != "x86_64":
        fail("publication requires the dedicated x86_64 Linux builder")

    android_root = Path(__file__).resolve().parents[1]
    outer_root = android_root.parents[1]
    line_root = outer_root / "kernel/android-6.18"
    common_root = outer_root / "kernel/common"
    sys.path.insert(0, str(android_root / "tools"))
    sys.path.insert(0, str(common_root / "artifacts"))
    from release_common import verify_kernel_config  # noqa: PLC0415
    from kernel_artifact import (  # noqa: PLC0415
        RELEASE_ID_RE,
        publish_artifact,
    )

    source = absolute_directory(args.source, "ACK source")
    build = absolute_directory(args.build, "ACK build")
    if not args.output_root.is_absolute():
        fail("output root must be an absolute builder-local path")
    output_root = args.output_root.resolve()
    if RELEASE_ID_RE.fullmatch(args.release_id) is None:
        fail("release ID has an unsafe format")
    objcopy = args.objcopy
    if not objcopy.is_absolute() or not objcopy.is_file() or \
            not os.access(objcopy, os.X_OK):
        fail("--objcopy must name an absolute executable")

    def check_release(release: str, metadata: dict[str, str]) -> None:
        if re.match(r"^6\.18(?:\.|-)", release) is None:
            fail("built kernel release is not ACK 6.18")

    destination = publish_artifact(
        fail,
        family="android-6.18",
        kernel_slice="arm64",
        page_size=4096,
        source=source,
        source_label="ACK source",
        build=build,
        output_root=output_root,
        release_id=args.release_id,
        objcopy=objcopy,
        common_root=common_root,
        metadata_path=line_root / "patches/metadata.env",
        check_release=check_release,
        verify_staged=lambda staging: verify_kernel_config(
            staging, android_root, enforce_tracked_policy=True
        ),
    )
    print(f"published Android ACK kernel artifact: {destination}")


if __name__ == "__main__":
    main()
