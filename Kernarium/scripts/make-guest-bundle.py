#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Assemble a Kernarium guest bundle from an installed Android release.

A bundle is a directory the application can boot on its own: the release
payload plus, once it has booted, the writable disk the guest accumulates. The
kernel is not part of it: the application carries an independently accepted
ACK artifact in its signed runtime framework.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True

BUNDLE_SUFFIX = ".kern"
MANIFEST = "release-manifest.json"
COMMAND_LINE = "boot/kernel-command-line.txt"
GUEST_DIRECTORY = "guest"
INSTANCE = "userdata.cow"
ROOT = Path(__file__).resolve().parents[2]
ACCEPTANCE = ROOT / "android/aosp-17/metadata/accepted-release.env"


def fail(message: str) -> None:
    raise SystemExit(f"Kernarium/scripts/make-guest-bundle.py: {message}")


def accepted_directory_name() -> str:
    pattern = re.compile(r"^ANDROID17_ACCEPTED_DIRECTORY_NAME='([^']+)'$", re.M)
    match = pattern.search(ACCEPTANCE.read_text(encoding="utf-8"))
    if match is None:
        fail(f"cannot read the accepted release identity from {ACCEPTANCE}")
    return match.group(1)


def base_disk(release: Path) -> Path:
    disks = sorted(
        entry
        for entry in (release / GUEST_DIRECTORY).glob("*.img")
        if entry.is_file()
    )
    if len(disks) != 1:
        fail(
            f"expected exactly one base image under {release / GUEST_DIRECTORY}, "
            f"found {len(disks)}"
        )
    return disks[0]


def copy(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    # A clone shares the compact seed's blocks until the bundle-local copy is
    # expanded at first boot.
    cloned = subprocess.run(
        ["cp", "-c", str(source), str(destination)],
        capture_output=True,
        check=False,
    )
    if cloned.returncode != 0:
        shutil.copyfile(source, destination)
    print(f"  {source.name} -> {destination}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--release", type=Path)
    parser.add_argument("--replace", action="store_true")
    arguments = parser.parse_args()

    release = arguments.release
    if release is None:
        release = ROOT / ".build/android/releases" / accepted_directory_name()
    release = release.resolve()
    if not release.is_dir():
        fail(f"the Android release is not installed: {release}")

    output = arguments.output
    if output.suffix != BUNDLE_SUFFIX:
        fail(f"a guest bundle must end in {BUNDLE_SUFFIX}: {output}")
    if output.exists():
        if not arguments.replace:
            fail(f"{output} already exists; pass --replace to rebuild it")
        # Replacing discards the writable disk with the payload it belongs to.
        shutil.rmtree(output)

    manifest = json.loads((release / MANIFEST).read_text(encoding="utf-8"))
    disk = base_disk(release)
    print(
        f"assembling {output.name} from {manifest['product_family']} "
        f"{manifest['release_id']}"
    )
    output.mkdir(parents=True)
    copy(release / MANIFEST, output / MANIFEST)
    copy(release / COMMAND_LINE, output / COMMAND_LINE)
    copy(disk, output / GUEST_DIRECTORY / disk.name)
    print(
        f"guest bundle ready: {output}\n"
        f"  the application creates {INSTANCE} inside it on first boot and "
        f"reopens it on every later boot"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
