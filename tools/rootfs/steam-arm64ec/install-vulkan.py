#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Install the receipt-bound Venus runtime from a sealed Vulkan guest tree."""
from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

REPOSITORY = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests/integration/gpu"))
from vulkan_cts_receipt import validate_receipt  # noqa: E402


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def install(source: Path, receipt_path: Path, root: Path) -> None:
    receipt = validate_receipt(receipt_path)
    records = receipt["mesa"]["files"] + [receipt["vulkan_loader"]["runtime"]]
    # Validate the complete selected payload before touching the fresh image.
    for record in records:
        path = source / record["path"].lstrip("/")
        if path.is_symlink() or path.stat().st_size != record["size"] or digest(path) != record["sha256"]:
            raise RuntimeError(f"Vulkan runtime does not match its receipt: {path}")
    for record in records:
        relative = record["path"].lstrip("/")
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.is_symlink():
            destination.unlink()
        shutil.copy2(source / relative, destination)
    links = dict(receipt["vulkan_loader"]["symlinks"])
    for library, target in (("libEGL_mesa.so.0", "libEGL_mesa.so.0.0.0"),
                            ("libGLX_mesa.so.0", "libGLX_mesa.so.0.0.0"),
                            ("libgbm.so.1", "libgbm.so.1.0.0")):
        links[f"/usr/lib/aarch64-linux-gnu/{library}"] = target
    for driver in ("kms_swrast", "swrast", "virtio_gpu", "zink"):
        links[f"/usr/lib/aarch64-linux-gnu/dri/{driver}_dri.so"] = "libdril_dri.so"
    for relative, target in links.items():
        path = root / relative.lstrip("/")
        path.unlink(missing_ok=True)
        path.symlink_to(target)
    # Force the intended device: a software ICD must not mask Venus failure.
    for path in (root / "usr/share/vulkan/icd.d").glob("*.json"):
        if path.name != "virtio_icd.json":
            path.unlink()
    subprocess.run(["chroot", str(root), "ldconfig"], check=True)
    for record in records:
        if ".so" not in record["path"]:
            continue
        result = subprocess.run(["chroot", str(root), "ldd", record["path"]],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
        if "not found" in result.stdout:
            raise RuntimeError(f"Vulkan runtime dependency missing: {result.stdout}")
    metadata = root / "usr/share/uml-steam/vulkan-runtime.json"
    metadata.parent.mkdir(parents=True, exist_ok=True)
    metadata.write_text(json.dumps({"source_receipt_sha256": digest(receipt_path),
                                   "mesa": receipt["mesa"], "protocol": receipt["protocol"],
                                   "vulkan_loader": receipt["vulkan_loader"],
                                   "installed_files": records, "symlinks": links}, indent=2) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    install(args.source.resolve(strict=True), args.receipt.resolve(strict=True), args.root.resolve(strict=True))


if __name__ == "__main__":
    main()
