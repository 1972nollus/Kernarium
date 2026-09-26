#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Write the release manifest for a completed starter-image build."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import datetime
import hashlib
import json
import platform
from pathlib import Path

from gpt import inspect

ARTIFACTS = (
    "uml-resize-initramfs.cpio.gz",
    "alpine-3.24.2-minimal.img",
    "debian-13-minimal.img",
    "debian-13-plasma-wayland.img",
)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--debian-version", required=True)
    arguments = parser.parse_args()
    output = arguments.output.resolve()
    products: dict[str, object] = {}
    for name in ARTIFACTS:
        path = output / name
        if not path.is_file() or path.is_symlink():
            raise SystemExit(f"starter manifest: missing artifact: {path}")
        stat = path.stat()
        entry: dict[str, object] = {
            "sha256": digest(path),
            "size": stat.st_size,
            "allocated_size": stat.st_blocks * 512,
        }
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
                "version": "2026.05.3",
                "sha256": "f43f1627082a45848f73083b3b8d59f32bb4c77a449a8ead8a2a1f91849dea00",
            },
            "busybox": {
                "version": "1.38.0",
                "sha256": "34f9ea6ff8636f2c9241153b9114eefa9e65674a45318ae1ef95bb5f31c53bb2",
            },
            "alpine": {
                "version": "3.24.2",
                "sha256": "9bf70a7f18ea44094cbb5f70c58f9af129c8214745743db0e68e5502cc2ce773",
            },
            "debian": {
                "suite": "trixie",
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
