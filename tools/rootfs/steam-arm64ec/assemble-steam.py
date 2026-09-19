#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Install the verified ARM64 Steam seed and its native client runtime."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import zipfile


def assemble(inputs: Path, steam: Path) -> None:
    receipt = json.loads((inputs / "inputs.json").read_text())
    steam.mkdir(parents=True)
    for entry in receipt["payloads"]:
        archive = Path(entry["file"])
        if archive.parent != inputs / "steam-packages":
            continue
        with archive.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != entry["sha256"]:
                raise RuntimeError(f"Steam package changed: {archive.name}")
        with zipfile.ZipFile(archive) as package:
            for member in package.infolist():
                path = PurePosixPath(member.filename)
                if path.is_absolute() or ".." in path.parts:
                    raise RuntimeError(f"unsafe Steam package path: {member.filename}")
        subprocess.run(["unzip", "-qo", str(archive), "-d", str(steam)], check=True)
    subprocess.run(["tar", "-xf", str(inputs / "steam-runtime.tar.xz"), "-C", str(steam)], check=True)
    for path in steam.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open("rb") as stream:
            magic = stream.read(4)
        if magic == b"\x7fELF" or magic.startswith(b"#!"):
            path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    package = steam / "package"
    package.mkdir(exist_ok=True)
    (package / "beta").write_text(receipt["steam_channel"] + "\n")
    shutil.copy2(inputs / "steam-client.manifest", package / f"steam_client_{receipt['steam_channel']}_linuxarm64.manifest")
    # The native updater owns its .installed manifest and first-run state.
    if not (steam / "steamrtarm64/steam").is_file():
        raise RuntimeError("Steam seed is missing the native ARM64 client")
    ibus = list(steam.glob("steam-runtime-steamrt-arm64/**/files/lib/aarch64-linux-gnu/libibus-1.0.so.5.*"))
    if len(ibus) != 1:
        raise RuntimeError("Steam client runtime must contain one libibus provider")
    import os
    shim = steam / "lib/aarch64-linux-gnu/libibus-1.0.so.5"
    shim.parent.mkdir(parents=True, exist_ok=True)
    if not shim.exists():
        shim.symlink_to(os.path.relpath(ibus[0], shim.parent))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputs", required=True, type=Path)
    parser.add_argument("--steam", required=True, type=Path)
    options = parser.parse_args()
    assemble(options.inputs.resolve(strict=True), options.steam.absolute())


if __name__ == "__main__":
    main()
