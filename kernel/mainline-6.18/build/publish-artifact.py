#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import sys
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"mainline kernel artifact publisher: {message}")


def absolute_directory(path: Path, label: str) -> Path:
    if not path.is_absolute():
        fail(f"{label} must be an absolute local path")
    resolved = path.resolve()
    if not resolved.is_dir():
        fail(f"{label} is not a directory: {resolved}")
    return resolved


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--page-size", choices=("4k", "16k"), required=True)
    parser.add_argument("--slice", choices=("arm64", "x86_64"), default="arm64")
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--release-id", required=True)
    parser.add_argument(
        "--objcopy",
        type=Path,
        required=True,
        help="llvm-objcopy of the toolchain that built the kernel; it strips "
        "the DWARF that only pahole consumes",
    )
    args = parser.parse_args()

    line_root = Path(__file__).resolve().parents[1]
    outer_root = line_root.parents[1]
    common_root = outer_root / "kernel/common"
    sys.path.insert(0, str(common_root / "artifacts"))
    from kernel_artifact import (  # noqa: PLC0415
        RELEASE_ID_RE,
        parse_env,
        publish_artifact,
    )

    source = absolute_directory(args.source, "kernel source")
    build = absolute_directory(args.build, "kernel build")
    if not args.output_root.is_absolute():
        fail("output root must be an absolute local path")
    output_root = args.output_root.resolve()
    if RELEASE_ID_RE.fullmatch(args.release_id) is None:
        fail("release ID has an unsafe format")
    objcopy = args.objcopy
    if not objcopy.is_absolute() or not objcopy.is_file() or not os.access(
        objcopy, os.X_OK
    ):
        fail("--objcopy must name an absolute executable")
    if args.slice == "x86_64" and args.page_size != "4k":
        fail("the x86_64 userspace slice supports only 4 KiB pages")

    metadata_path = line_root / "patches/metadata.env"
    if args.slice == "x86_64":
        common_metadata = parse_env(metadata_path)
        metadata_path = line_root / "slices/x86_64/patches/metadata.env"
        metadata = parse_env(metadata_path)
        if metadata.get("UML_KERNEL_SLICE") != "x86_64":
            fail("x86_64 patch metadata has the wrong slice identity")
        inherited_keys = (
            "UML_KERNEL_RELEASE_VERSION",
            "UML_KERNEL_UPSTREAM_REPOSITORY",
            "UML_KERNEL_UPSTREAM_BRANCH",
            "UML_KERNEL_UPSTREAM_COMMIT",
            "UML_KERNEL_UPSTREAM_TREE",
            "UML_KERNEL_BASE_REF",
        )
        mismatched = [
            key
            for key in inherited_keys
            if metadata.get(key) != common_metadata.get(key)
        ]
        if mismatched:
            fail(
                "x86_64 patch metadata differs from common identity: "
                + ", ".join(mismatched)
            )

    def check_release(release: str, metadata: dict[str, str]) -> None:
        if not release.startswith(metadata["UML_KERNEL_RELEASE_VERSION"]):
            fail("built kernel release does not match tracked release version")

    destination = publish_artifact(
        fail,
        family="mainline-6.18",
        kernel_slice=args.slice,
        page_size=16384 if args.page_size == "16k" else 4096,
        source=source,
        source_label="kernel source",
        build=build,
        output_root=output_root,
        release_id=args.release_id,
        objcopy=objcopy,
        common_root=common_root,
        metadata_path=metadata_path,
        check_release=check_release,
    )
    print(f"published {args.slice} mainline kernel artifact: {destination}")


if __name__ == "__main__":
    main()
