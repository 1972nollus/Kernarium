#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from release_common import (  # noqa: E402
    PAYLOAD_SPECS,
    fail,
    resolve_zstd,
    verify_release,
)


def reject_nonlocal_syntax(raw: str) -> None:
    if "://" in raw or (":" in raw and not raw.startswith("/")) or "@" in raw:
        fail("release source must use local filesystem path syntax")


def copy_declared(source: Path, staging: Path) -> None:
    paths = ["release-manifest.json", *sorted(PAYLOAD_SPECS)]
    for relative in paths:
        source_path = source / relative
        destination = staging / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source_path, destination, follow_symlinks=False)
        shutil.copymode(source_path, destination, follow_symlinks=False)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--destination-root", type=Path)
    parser.add_argument("--acceptance", type=Path)
    parser.add_argument("--zstd", type=Path)
    args = parser.parse_args()

    reject_nonlocal_syntax(args.source)
    source = Path(args.source)
    if not source.is_absolute():
        fail("release source must be an absolute local path")

    android_root = Path(__file__).resolve().parents[1]
    outer_root = android_root.parents[1]
    destination_root = args.destination_root or outer_root / ".build/android/releases"
    acceptance = args.acceptance or android_root / "metadata/accepted-release.env"
    if not destination_root.is_absolute():
        fail("destination root must be an absolute local path")
    if not acceptance.is_absolute():
        acceptance = (Path.cwd() / acceptance).resolve()
    zstd = resolve_zstd(args.zstd)

    manifest = verify_release(
        source,
        android_root,
        acceptance_path=acceptance,
        zstd=zstd,
    )
    destination_root.mkdir(parents=True, exist_ok=True)
    if destination_root.is_symlink():
        fail("destination root must not be a symlink")
    destination = destination_root / manifest["directory_name"]
    if destination.exists() or destination.is_symlink():
        fail("accepted release is already installed")

    staging_path = Path(tempfile.mkdtemp(
        prefix=f".{manifest['directory_name']}.", dir=destination_root
    ))
    try:
        copy_declared(source, staging_path)
        expanded_disk = staging_path / "guest/android-17-uml-arm64.img"
        archive = staging_path / "guest/android-17-uml-arm64.img.zst"
        result = subprocess.run(
            [
                str(zstd),
                "-q",
                "-d",
                "--sparse",
                "--no-progress",
                str(archive),
                "-o",
                str(expanded_disk),
            ],
            check=False,
        )
        if result.returncode != 0:
            fail("cannot expand the Android guest disk archive")
        verify_release(
            staging_path,
            android_root,
            acceptance_path=acceptance,
            zstd=zstd,
            expanded_disk=expanded_disk,
            allow_expanded_disk=True,
        )
        os.replace(staging_path, destination)
    except BaseException:
        shutil.rmtree(staging_path, ignore_errors=True)
        raise

    print(f"installed Android release: {destination}")


if __name__ == "__main__":
    main()
