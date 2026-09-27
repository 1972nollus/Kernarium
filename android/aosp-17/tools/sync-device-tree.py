#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Synchronize a checked Kernarium device tree into an AOSP or OrangeFox workspace."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import shutil
import stat
from dataclasses import dataclass
from pathlib import Path, PurePosixPath


DESTINATION = PurePosixPath("device/kernarium/darwin")
GENERATED_KERNEL = PurePosixPath("prebuilt/kernel")


@dataclass(frozen=True)
class Tree:
    source: str
    workspace: str
    label: str
    prebuilt_kernel: bool


TREES = {
    "overlay": Tree("overlay", "AOSP source", "device overlay", False),
    "orangefox": Tree(
        "third_party/orangefox/device", "OrangeFox source", "OrangeFox device tree", True
    ),
}


def fail(message: str) -> None:
    raise SystemExit(f"device-tree sync: {message}")


def checked_root(path: Path, label: str) -> Path:
    if not path.is_absolute():
        fail(f"{label} must be absolute")
    resolved = path.resolve()
    if resolved == Path("/") or not resolved.is_dir():
        fail(f"{label} is not a directory: {resolved}")
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
                fail(f"non-regular device entry is forbidden: {relative}")
            result[relative] = (
                "file",
                stat.S_IMODE(path.stat().st_mode),
                path.read_bytes(),
            )
    return result


def verify(source: Path, destination: Path, allow_kernel: bool) -> None:
    if not destination.is_dir() or destination.is_symlink():
        fail(f"destination is missing or unsafe: {destination}")
    source_entries = inventory(source)
    destination_entries = inventory(destination)
    if allow_kernel and GENERATED_KERNEL in destination_entries:
        kind, mode, payload = destination_entries.pop(GENERATED_KERNEL)
        if kind != "file" or not mode & 0o111 or not payload:
            fail("generated prebuilt kernel is not a nonempty executable regular file")
        destination_entries.pop(GENERATED_KERNEL.parent, None)
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
    # copytree follows links, so reject a linked source entry before copying.
    inventory(source)
    if destination.exists() or destination.is_symlink():
        if destination.is_symlink() or not destination.is_dir():
            fail(f"destination is not a regular directory: {destination}")
        # Inspect before removing so a stale symbolic link cannot redirect any
        # operation outside the one exact device subtree.
        inventory(destination)
        shutil.rmtree(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, destination, copy_function=shutil.copy2)
    verify(source, destination, False)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--tree", choices=sorted(TREES), required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--allow-prebuilt-kernel", action="store_true")
    args = parser.parse_args()
    tree = TREES[args.tree]
    if args.allow_prebuilt_kernel and not tree.prebuilt_kernel:
        fail(f"--allow-prebuilt-kernel is invalid for the {args.tree} tree")
    if args.allow_prebuilt_kernel and not args.verify:
        fail("--allow-prebuilt-kernel is valid only with --verify")

    android_root = checked_root(args.android_root, "Android policy root")
    workspace = checked_root(args.workspace, tree.workspace)
    if not (workspace / ".repo/manifests").is_dir():
        fail(f"{tree.workspace} lacks Repo metadata")
    source = android_root / tree.source
    if not source.is_dir() or source.is_symlink():
        fail(f"checked {tree.label} is missing or unsafe")
    destination = workspace / Path(DESTINATION)
    try:
        destination.parent.resolve().relative_to(workspace)
    except ValueError:
        fail(f"device destination escapes the {tree.workspace}")

    if args.verify:
        verify(source, destination, args.allow_prebuilt_kernel)
        print(f"verified exact {tree.label}: {destination}")
    else:
        synchronize(source, destination)
        print(f"synchronized exact {tree.label}: {destination}")


if __name__ == "__main__":
    main()
