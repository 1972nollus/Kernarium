#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import shutil
import stat
from pathlib import Path, PurePosixPath


DESTINATION = PurePosixPath("device/kernarium/darwin")


def fail(message: str) -> None:
    raise SystemExit(f"Android 17 device-overlay sync: {message}")


def checked_root(path: Path, marker: str) -> Path:
    if not path.is_absolute():
        fail(f"{marker} must be absolute")
    resolved = path.resolve()
    if resolved == Path("/") or not resolved.is_dir():
        fail(f"{marker} is not a directory: {resolved}")
    return resolved


def inventory(root: Path) -> dict[PurePosixPath, tuple[str, int, bytes | None]]:
    result: dict[PurePosixPath, tuple[str, int, bytes | None]] = {}
    for current, directories, files in os.walk(root, topdown=True, followlinks=False):
        current_path = Path(current)
        relative_directory = current_path.relative_to(root)
        for name in sorted(directories):
            path = current_path / name
            relative = PurePosixPath((relative_directory / name).as_posix())
            if path.is_symlink():
                fail(f"symbolic links are forbidden: {relative}")
            result[relative] = ("directory", stat.S_IMODE(path.stat().st_mode), None)
        for name in sorted(files):
            path = current_path / name
            relative = PurePosixPath((relative_directory / name).as_posix())
            if path.is_symlink() or not path.is_file():
                fail(f"non-regular overlay entry is forbidden: {relative}")
            result[relative] = (
                "file",
                stat.S_IMODE(path.stat().st_mode),
                path.read_bytes(),
            )
    return result


def verify(source: Path, destination: Path) -> None:
    if not destination.is_dir() or destination.is_symlink():
        fail(f"destination is missing or unsafe: {destination}")
    source_entries = inventory(source)
    destination_entries = inventory(destination)
    if source_entries.keys() != destination_entries.keys():
        missing = sorted(str(path) for path in source_entries.keys() - destination_entries.keys())
        extra = sorted(str(path) for path in destination_entries.keys() - source_entries.keys())
        detail = []
        if missing:
            detail.append(f"missing={','.join(missing[:8])}")
        if extra:
            detail.append(f"extra={','.join(extra[:8])}")
        fail(f"destination inventory differs ({'; '.join(detail)})")
    for relative, source_entry in source_entries.items():
        if destination_entries[relative] != source_entry:
            fail(f"destination entry differs: {relative}")


def synchronize(source: Path, destination: Path) -> None:
    if destination.exists() or destination.is_symlink():
        if destination.is_symlink() or not destination.is_dir():
            fail(f"destination is not a regular directory: {destination}")
        # Inspect before removing so a stale symbolic link cannot redirect any
        # operation outside the one exact device subtree.
        inventory(destination)
        shutil.rmtree(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, destination, copy_function=shutil.copy2)
    verify(source, destination)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--aosp-source", type=Path, required=True)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()

    android_root = checked_root(args.android_root, "Android policy root")
    aosp_source = checked_root(args.aosp_source, "AOSP source")
    if not (aosp_source / ".repo/manifests").is_dir():
        fail("AOSP source lacks Repo metadata")
    source = android_root / "overlay"
    if not source.is_dir() or source.is_symlink():
        fail("checked device overlay is missing or unsafe")
    destination = aosp_source / Path(DESTINATION)
    try:
        destination.parent.resolve().relative_to(aosp_source)
    except ValueError:
        fail("device destination escapes the AOSP source")

    if args.verify:
        verify(source, destination)
        print(f"verified exact device overlay: {destination}")
    else:
        synchronize(source, destination)
        print(f"synchronized exact device overlay: {destination}")


if __name__ == "__main__":
    main()
