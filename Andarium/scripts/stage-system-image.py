#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stage the accepted Android release as the system image Andarium carries.

The application bundles the staged directory as it stands: the guest's disk as
the zstd archive the release ships, the kernel command line, the release
manifest and the disk's partition layout. The staged files are copies of an
accepted release, which is checked against its own manifest; this script
neither builds nor signs anything.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ACCEPTANCE = ROOT / "android/lineage-24/metadata/accepted-release.env"
DEFAULT_RELEASES = ROOT / ".build/android/releases"
DEFAULT_OUTPUT = ROOT / ".build/andarium-system"
_ARTIFACTS = str(ROOT / "kernel/common/artifacts")
if _ARTIFACTS not in sys.path:
    sys.path.insert(0, _ARTIFACTS)
import contract_checks  # noqa: E402

MANIFEST = "release-manifest.json"
COMMAND_LINE = "boot/kernel-command-line.txt"
PARTITION_LAYOUT = "guest/partition-layout.json"
ARCHIVE_ROLE = "guest-disk-archive"
ZSTD_MAGIC = bytes.fromhex("28b52ffd")
GUEST_PAGE_SIZE = 4096


class StageError(Exception):
    pass


def fail(message: str) -> None:
    raise SystemExit(f"Andarium/scripts/stage-system-image.py: {message}")


def read_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise StageError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise StageError(f"{path} is not a JSON object")
    return value


def zstd_content_size(header: bytes) -> int | None:
    """The decompressed size a zstd frame header declares, if it declares one."""
    if len(header) < 6 or header[:4] != ZSTD_MAGIC:
        raise StageError("the disk archive is not a zstd frame")
    descriptor = header[4]
    size_flag = descriptor >> 6
    single_segment = bool(descriptor & 0x20)
    dictionary_bytes = (0, 1, 2, 4)[descriptor & 0x03]
    offset = 5 + (0 if single_segment else 1) + dictionary_bytes
    width = (1 if single_segment else 0, 2, 4, 8)[size_flag]
    if width == 0:
        return None
    field = header[offset:offset + width]
    if len(field) != width:
        raise StageError("the zstd frame header is truncated")
    size = int.from_bytes(field, "little")
    return size + 256 if width == 2 else size


def check_release(release: Path, manifest: dict, accepted: dict[str, str] | None) -> Path:
    """Checks the release and returns its disk archive."""
    if manifest.get("acceptance") != "accepted":
        raise StageError("the release is not accepted")
    android = manifest.get("android")
    kernel = manifest.get("kernel")
    if not isinstance(android, dict) or not isinstance(kernel, dict):
        raise StageError("the manifest has no android or kernel section")
    if kernel.get("page_size") != GUEST_PAGE_SIZE:
        raise StageError(f"the release is not a {GUEST_PAGE_SIZE}-byte-page release")
    if not str(manifest.get("product_family", "")).startswith("lineage-"):
        raise StageError("the release is not a LineageOS product family")
    if accepted is not None:
        expected = {
            "release_id": accepted["LINEAGE24_ACCEPTED_RELEASE_ID"],
            "product_family": accepted["LINEAGE24_ACCEPTED_PRODUCT_FAMILY"],
        }
        for key, value in expected.items():
            if manifest.get(key) != value:
                raise StageError(f"{key} {manifest.get(key)!r} is not the accepted {value!r}")
        if str(android.get("major")) != accepted["LINEAGE24_ACCEPTED_ANDROID_MAJOR"]:
            raise StageError("the Android major version is not the accepted one")

    inventory = manifest.get("inventory")
    if not isinstance(inventory, list):
        raise StageError("the manifest has no inventory")
    by_path = {entry.get("path"): entry for entry in inventory if isinstance(entry, dict)}
    archives = [entry for entry in by_path.values() if entry.get("role") == ARCHIVE_ROLE]
    if len(archives) != 1:
        raise StageError(f"expected one {ARCHIVE_ROLE} in the inventory, found {len(archives)}")
    archive = release / str(archives[0]["path"])
    for path in (archive, release / COMMAND_LINE, release / PARTITION_LAYOUT):
        if not path.is_file() or path.is_symlink():
            raise StageError(f"missing or unsafe release file: {path}")
    for relative in (archives[0]["path"], COMMAND_LINE, PARTITION_LAYOUT):
        entry = by_path.get(relative)
        if entry is None:
            raise StageError(f"{relative} is not in the manifest inventory")
        if (release / relative).stat().st_size != entry.get("size"):
            raise StageError(f"{relative} does not match the size in the manifest")

    layout = read_json(release / PARTITION_LAYOUT)
    with archive.open("rb") as handle:
        declared = zstd_content_size(handle.read(18))
    if declared is not None and declared != layout.get("disk_size_bytes"):
        raise StageError(
            f"the archive expands to {declared} bytes but the partition layout "
            f"says {layout.get('disk_size_bytes')}"
        )
    return archive


def copy(source: Path, destination: Path) -> None:
    # A clone costs nothing on APFS; elsewhere the bytes are copied.
    cloned = subprocess.run(
        ["cp", "-c", str(source), str(destination)], capture_output=True, check=False
    )
    if cloned.returncode != 0:
        shutil.copyfile(source, destination)


def stage(release: Path, manifest: dict, archive: Path, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix=f".{output.name}.", dir=output.parent))
    try:
        copy(archive, scratch / archive.name)
        copy(release / COMMAND_LINE, scratch / "kernel-command-line.txt")
        copy(release / MANIFEST, scratch / MANIFEST)
        copy(release / PARTITION_LAYOUT, scratch / "partition-layout.json")
        previous = None
        if output.exists():
            previous = Path(tempfile.mkdtemp(prefix=f".{output.name}.old.", dir=output.parent))
            os.rmdir(previous)
            os.rename(output, previous)
        try:
            os.rename(scratch, output)
        except OSError:
            if previous is not None:
                os.rename(previous, output)
            raise
        if previous is not None:
            shutil.rmtree(previous, ignore_errors=True)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--release", type=Path, help="a release directory (default: the accepted release)")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--acceptance", type=Path, default=DEFAULT_ACCEPTANCE, help=argparse.SUPPRESS)
    parser.add_argument("--releases", type=Path, default=DEFAULT_RELEASES, help=argparse.SUPPRESS)
    arguments = parser.parse_args(argv)

    try:
        accepted = None
        release = arguments.release
        if release is None:
            accepted = contract_checks.parse_assignments(arguments.acceptance, StageError)
            release = arguments.releases / accepted["LINEAGE24_ACCEPTED_DIRECTORY_NAME"]
        release = release.resolve()
        if not release.is_dir():
            raise StageError(f"the Android release is not installed: {release}")
        manifest = read_json(release / MANIFEST)
        archive = check_release(release, manifest, accepted)
        stage(release, manifest, archive, arguments.output)
    except StageError as error:
        fail(str(error))
    print(f"staged {manifest['product_family']} {manifest['release_id']}: {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
