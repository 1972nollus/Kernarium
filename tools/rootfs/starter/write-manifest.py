#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Write the release manifest for a completed starter-image build.

build-all.sh owns the source pins and passes them here, so the manifest
records the sources the build actually verified."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import datetime
import hashlib
import json
import platform
from pathlib import Path

from gpt import inspect


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    for source in ("buildroot", "busybox", "alpine"):
        parser.add_argument(f"--{source}-version", required=True)
        parser.add_argument(f"--{source}-sha256", required=True)
    parser.add_argument("--debian-suite", required=True)
    parser.add_argument("--debian-version", required=True)
    arguments = parser.parse_args()
    output = arguments.output.resolve()
    boot_bases = {
        "uml-boot-base-aarch64.cpio.gz": "aarch64",
        "uml-boot-base-x86_64.cpio.gz": "x86_64",
    }
    artifacts = (
        *boot_bases,
        f"alpine-{arguments.alpine_version}-minimal.img",
        "debian-13-minimal.img",
        "debian-13-plasma-wayland.img",
    )
    products: dict[str, object] = {}
    for name in artifacts:
        path = output / name
        if not path.is_file() or path.is_symlink():
            raise SystemExit(f"starter manifest: missing artifact: {path}")
        stat = path.stat()
        entry: dict[str, object] = {
            "sha256": digest(path),
            "size": stat.st_size,
            "allocated_size": stat.st_blocks * 512,
        }
        if name in boot_bases:
            entry["architecture"] = boot_bases[name]
        if path.suffix == ".img":
            disk = inspect(path)
            disk.pop("image", None)
            entry["disk"] = disk
            inventory = output / f"{name}.packages.tsv"
            if not inventory.is_file() or inventory.is_symlink():
                raise SystemExit(
                    f"starter manifest: missing package inventory: {inventory}"
                )
            entry["package_inventory"] = {
                "path": inventory.name,
                "sha256": digest(inventory),
                "size": inventory.stat().st_size,
            }
        products[name] = entry
    manifest = {
        "schema_version": 1,
        "architecture": "aarch64",
        "built_at_utc": datetime.datetime.now(datetime.timezone.utc)
        .isoformat(timespec="seconds")
        .replace("+00:00", "Z"),
        "builder": {
            "kernel": platform.release(),
            "machine": platform.machine(),
            "system": platform.system(),
        },
        "sources": {
            "buildroot": {
                "version": arguments.buildroot_version,
                "sha256": arguments.buildroot_sha256,
            },
            "busybox": {
                "version": arguments.busybox_version,
                "sha256": arguments.busybox_sha256,
            },
            "alpine": {
                "version": arguments.alpine_version,
                "sha256": arguments.alpine_sha256,
            },
            "debian": {
                "suite": arguments.debian_suite,
                "version": arguments.debian_version,
            },
        },
        "artifacts": products,
    }
    path = output / "release-manifest.json"
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"wrote starter release manifest: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
