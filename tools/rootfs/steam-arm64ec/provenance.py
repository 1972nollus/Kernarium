#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Record actual producer sources and the installed Proton binary identities."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
from pathlib import Path


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputs", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    repository = here.parents[2]
    recorded = json.loads((args.inputs / "inputs.json").read_text())
    sources = [repository / "darwin/runtime/wine_cpu_protocol.h",
               repository / "dependencies/prepare.sh", here.parent / "wine-arm64ec/verify-pe.py"]
    for directory in (here, here.parent / "wine-arm64ec/bridge", here.parent / "wine-arm64ec/wow64-bridge",
                      here.parent / "wine-arm64ec/smoke", repository / "dependencies/steam-arm64ec"):
        sources += sorted(path for path in directory.rglob("*") if path.is_file() and path.name != "README.md")
    recorded["producer_sources"] = {path.relative_to(repository).as_posix(): digest(path) for path in sources}
    graphics = args.root / "usr/share/uml-steam/vulkan-runtime.json"
    if graphics.is_file():
        recorded["vulkan_runtime"] = json.loads(graphics.read_text())
    proton = args.root / "opt/proton-uml-arm64ec"
    recorded["proton_version"] = (proton / "version").read_text().strip()
    recorded["proton_binaries"] = {
        path.relative_to(proton).as_posix(): digest(path)
        for path in sorted(proton.rglob("*")) if path.is_file() and not path.is_symlink()
    }
    args.output.write_text(json.dumps(recorded, indent=2) + "\n")


if __name__ == "__main__":
    main()
