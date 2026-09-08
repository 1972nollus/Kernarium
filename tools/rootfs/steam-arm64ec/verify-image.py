#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Audit native Linux execution, Windows ARM64EC translation, and gaming files."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import stat


HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("wine_pe", HERE.parent / "wine-arm64ec/verify-pe.py")
assert spec is not None and spec.loader is not None
wine_pe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wine_pe)

STEAM = "usr/share/uml-steam/home/uml/.local/share/Steam/"
PROTON = "opt/proton-uml-arm64ec/"
STEAM_FOREIGN = tuple(STEAM + name + "/" for name in (
    "linux32", "linux64", "steamrt32", "steamrt64", "ubuntu12_32", "ubuntu12_64",
))
ELF_MACHINES = {3: "I386", 62: "AMD64", 183: "AArch64"}
REQUIRED_ELF = (
    "usr/local/libexec/kernarium-screen",
    "usr/bin/lsof",
    "usr/games/gamescope",
    "usr/games/gamescopereaper",
    PROTON + "files/bin-arm64/wine",
    PROTON + "files/bin-arm64/wineserver",
    STEAM + "steamrtarm64/steam",
    STEAM + "steamrtarm64/steamwebhelper",
)
REQUIRED_LIBRARIES = (
    "usr/lib/aarch64-linux-gnu/libcairo.so.2",
    "usr/lib/aarch64-linux-gnu/libX11.so.6",
    "usr/lib/aarch64-linux-gnu/libdrm.so.2",
    "usr/lib/aarch64-linux-gnu/libgtk-x11-2.0.so.0",
    "usr/lib/aarch64-linux-gnu/libopenal.so.1",
    "usr/lib/aarch64-linux-gnu/libnm.so.0",
)
REQUIRED_PE = {
    PROTON + "files/lib/wine/aarch64-windows/xtajit64.dll": "ARM64EC",
    PROTON + "files/lib/wine/aarch64-windows/xtajit.dll": "ARM64",
    PROTON + "files/lib/wine/dxvk/aarch64-windows/d3d11.dll": "ARM64EC",
    PROTON + "files/lib/wine/dxvk/aarch64-windows/dxgi.dll": "ARM64EC",
    PROTON + "files/lib/wine/vkd3d-proton/aarch64-windows/d3d12.dll": "ARM64EC",
    "opt/uml-wine-smoke/x86_64-exit-42.exe": "AMD64",
    "opt/uml-wine-smoke/i386-exit-43.exe": "I386",
}


def inventory(root: Path) -> list[tuple[str, str, str]]:
    entries = []
    observed = {}
    for directory, _, names in os.walk(root, followlinks=False):
        for name in names:
            path = Path(directory) / name
            if not stat.S_ISREG(path.lstat().st_mode):
                continue
            relative = path.relative_to(root).as_posix()
            if "\n" in relative or "\t" in relative:
                raise RuntimeError(f"binary path is not TSV-safe: {relative!r}")
            if name.lower() in {"libarm64ecfex.dll", "libwow64fex.dll", "libfexcore.so"}:
                raise RuntimeError(f"unexpected guest FEX engine: {relative}")
            with path.open("rb") as stream:
                header = stream.read(20)
            if header.startswith(b"\x7fELF"):
                machine = int.from_bytes(header[18:20], "little")
                if len(header) != 20 or header[5] != 1 or machine not in ELF_MACHINES:
                    raise RuntimeError(f"invalid ELF payload: {relative}")
                if machine != 183 and not (
                    relative.startswith(STEAM_FOREIGN)
                    or relative == STEAM + "bin/hardwareupdater/hardwareupdater.x86_64"
                ):
                    raise RuntimeError(f"unexpected foreign ELF payload: {relative}")
                kind, architecture = "ELF", ELF_MACHINES[machine]
            else:
                machine = wine_pe.pe_machine(path)
                if machine is None:
                    continue
                if machine not in wine_pe.MACHINES:
                    raise RuntimeError(f"unsupported PE payload: {relative}")
                kind, architecture = "PE", wine_pe.MACHINES[machine]
            observed[relative] = (kind, architecture)
            entries.append((relative, kind, architecture))
    for relative in REQUIRED_ELF:
        if observed.get(relative) != ("ELF", "AArch64"):
            raise RuntimeError(f"required native Linux executable is absent or foreign: {relative}")
    for relative in REQUIRED_LIBRARIES:
        resolved = (root / relative).resolve()
        if not resolved.is_relative_to(root.resolve()) or observed.get(
                resolved.relative_to(root.resolve()).as_posix()) != ("ELF", "AArch64"):
            raise RuntimeError(f"required native Linux library is absent or foreign: {relative}")
    for relative, architecture in REQUIRED_PE.items():
        if observed.get(relative) != ("PE", architecture):
            raise RuntimeError(f"required {architecture} Windows image is absent or invalid: {relative}")
    decoder_rule = root / "etc/udev/rules.d/70-uml-virtio-video.rules"
    if not decoder_rule.is_file() or 'SYMLINK+="video-dec%n"' not in decoder_rule.read_text():
        raise RuntimeError("V4L2 decoder discovery rule is absent or invalid")
    version = hashlib.sha256((root / PROTON / "version").read_bytes()).hexdigest()
    for relative in ("dist.lock", "files/share/default_pfx", "files/share/default_pfx_arm64"):
        path = root / PROTON / relative
        expected = f"/home/uml/.local/share/proton-{version}-{path.name}"
        if not path.is_symlink() or os.readlink(path) != expected:
            raise RuntimeError(f"Proton writable state is not on the data partition: {relative}")
    wine_inf = (root / PROTON / "files/share/wine/wine.inf").read_text()
    for name in ("libarm64ecfex.dll", "libwow64fex.dll"):
        if name in wine_inf:
            raise RuntimeError(f"Wine registry still selects {name}")
    for name in ("xtajit64.dll", "xtajit.dll"):
        if name not in wine_inf:
            raise RuntimeError(f"Wine registry does not select {name}")
    for executable in ("usr/bin/startxfce4", "usr/bin/gnome-session", "usr/bin/startplasma-wayland"):
        if (root / executable).exists():
            raise RuntimeError(f"desktop environment was installed: {executable}")
    if not list((root / "usr/share/vulkan/icd.d").glob("*.json")):
        raise RuntimeError("Mesa Vulkan ICDs are absent")
    graphics = root / "usr/share/uml-steam/vulkan-runtime.json"
    if graphics.is_file():
        recorded = json.loads(graphics.read_text())
        for record in recorded["installed_files"]:
            path = root / record["path"].lstrip("/")
            with path.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            if path.is_symlink() or digest != record["sha256"]:
                raise RuntimeError(f"installed Vulkan runtime differs: {path}")
        for relative, target in recorded["symlinks"].items():
            if os.readlink(root / relative.lstrip("/")) != target:
                raise RuntimeError(f"installed Vulkan link differs: {relative}")
        if sorted(path.name for path in (root / "usr/share/vulkan/icd.d").glob("*.json")) != ["virtio_icd.json"]:
            raise RuntimeError("Venus image contains an alternate Vulkan ICD")
    return sorted(entries)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    entries = inventory(args.root.resolve(strict=True))
    args.output.write_text("path\tformat\tarchitecture\n" + "".join("\t".join(row) + "\n" for row in entries))
    print(f"Verified {len(entries)} Steam/Proton binaries")


if __name__ == "__main__":
    main()
