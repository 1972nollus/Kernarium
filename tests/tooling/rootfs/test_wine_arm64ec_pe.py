#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify ARM64EC CHPE classification and the rootfs PE allowlist."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


PE_OFFSET = 0x80
OPTIONAL_HEADER_SIZE = 0xF0
IMAGE_BASE = 0x140000000
SECTION_RVA = 0x1000
SECTION_RAW_OFFSET = 0x200
LOAD_CONFIG_RVA = SECTION_RVA
METADATA_RVA = SECTION_RVA + 0x100
METADATA_RAW_OFFSET = SECTION_RAW_OFFSET + 0x100


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def write_pe(path: Path, machine: int) -> None:
    image = bytearray(PE_OFFSET + 24)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 60, PE_OFFSET)
    image[PE_OFFSET : PE_OFFSET + 4] = b"PE\0\0"
    struct.pack_into("<H", image, PE_OFFSET + 4, machine)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(image)


def write_arm64ec_pe(path: Path, metadata_version: int = 2) -> None:
    image = bytearray(0x600)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 60, PE_OFFSET)
    image[PE_OFFSET : PE_OFFSET + 4] = b"PE\0\0"
    coff_offset = PE_OFFSET + 4
    struct.pack_into(
        "<HHIIIHH",
        image,
        coff_offset,
        0x8664,
        1,
        0,
        0,
        0,
        OPTIONAL_HEADER_SIZE,
        0,
    )

    optional_offset = PE_OFFSET + 24
    struct.pack_into("<H", image, optional_offset, 0x20B)
    struct.pack_into("<Q", image, optional_offset + 24, IMAGE_BASE)
    struct.pack_into("<I", image, optional_offset + 56, 0x3000)
    struct.pack_into("<I", image, optional_offset + 60, 0x200)
    struct.pack_into("<I", image, optional_offset + 108, 16)
    load_directory_offset = optional_offset + 112 + 10 * 8
    struct.pack_into(
        "<II", image, load_directory_offset, LOAD_CONFIG_RVA, 0xD0
    )

    section_offset = optional_offset + OPTIONAL_HEADER_SIZE
    image[section_offset : section_offset + 8] = b".rdata\0\0"
    struct.pack_into(
        "<IIII",
        image,
        section_offset + 8,
        0x1000,
        SECTION_RVA,
        0x400,
        SECTION_RAW_OFFSET,
    )
    struct.pack_into("<I", image, SECTION_RAW_OFFSET, 0xD0)
    struct.pack_into(
        "<Q",
        image,
        SECTION_RAW_OFFSET + 0xC8,
        IMAGE_BASE + METADATA_RVA,
    )
    struct.pack_into("<I", image, METADATA_RAW_OFFSET, metadata_version)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(image)


def run_verifier(
    verifier: Path, root: Path, output: Path
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(verifier), "--root", str(root),
         "--output", str(output)],
        check=False,
        capture_output=True,
        text=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--verifier", required=True, type=Path)
    parser.add_argument("--builder", required=True, type=Path)
    arguments = parser.parse_args()

    builder = arguments.builder.read_text(encoding="utf-8")
    package_start = builder.index(
        "apt-get install -y --no-install-recommends"
    )
    package_end = builder.index(
        "dpkg --print-foreign-architectures", package_start
    )
    require(
        "e2fsprogs" in builder[package_start:package_end],
        "ARM64EC Wine rootfs lacks the ext4 formatter required by Kernarium",
    )
    require(
        "xserver-xorg-input-libinput" in builder[package_start:package_end],
        "ARM64EC Wine rootfs lacks the Xorg driver for host input devices",
    )

    with tempfile.TemporaryDirectory(prefix="uml-wine-pe-test-") as name:
        temporary = Path(name)
        root = temporary / "root"
        amd64_smoke = root / "opt/uml-wine-smoke/x86_64-exit-42.exe"
        i386_smoke = root / "opt/uml-wine-smoke/i386-exit-43.exe"
        bridge = root / "opt/wine/lib/wine/aarch64-windows/xtajit64.dll"
        wow64_bridge = root / "opt/wine/lib/wine/aarch64-windows/xtajit.dll"
        i386_runtime = (
            root / "opt/wine-arm64ec/lib/wine/i386-windows/ntdll.dll"
        )
        hybrid = root / "opt/wine/lib/wine/aarch64-windows/dpnsvr.exe"
        native = root / "opt/wine/lib/wine/aarch64-windows/arm64.dll"
        arm64x = root / "opt/wine/lib/wine/aarch64-windows/arm64x.dll"
        write_pe(amd64_smoke, 0x8664)
        write_pe(i386_smoke, 0x014C)
        write_arm64ec_pe(bridge)
        write_pe(wow64_bridge, 0xAA64)
        write_pe(i386_runtime, 0x014C)
        write_arm64ec_pe(hybrid)
        write_pe(native, 0xAA64)
        write_pe(arm64x, 0xA64E)

        output = temporary / "inventory.tsv"
        result = run_verifier(arguments.verifier, root, output)
        require(result.returncode == 0, result.stderr)
        entries = set(output.read_text(encoding="utf-8").splitlines())
        require(
            entries
            == {
                "opt/uml-wine-smoke/x86_64-exit-42.exe\tAMD64",
                "opt/uml-wine-smoke/i386-exit-43.exe\tI386",
                "opt/wine/lib/wine/aarch64-windows/arm64.dll\tARM64",
                "opt/wine/lib/wine/aarch64-windows/arm64x.dll\tARM64X",
                "opt/wine/lib/wine/aarch64-windows/dpnsvr.exe\tARM64EC",
                "opt/wine/lib/wine/aarch64-windows/xtajit.dll\tARM64",
                "opt/wine/lib/wine/aarch64-windows/xtajit64.dll\tARM64EC",
                "opt/wine-arm64ec/lib/wine/i386-windows/ntdll.dll\tI386",
            },
            f"unexpected inventory: {entries}",
        )

        rogue = root / "opt/wine/lib/wine/aarch64-windows/rogue.exe"
        write_pe(rogue, 0x8664)
        result = run_verifier(arguments.verifier, root, output)
        require(result.returncode == 1, "unexpected AMD64 PE was accepted")
        require(
            "unexpected AMD64 PE payload" in result.stderr,
            f"wrong unexpected-AMD64 error: {result.stderr}",
        )
        rogue.unlink()

        rogue_i386 = root / "opt/wine/lib/wine/aarch64-windows/rogue32.exe"
        write_pe(rogue_i386, 0x014C)
        result = run_verifier(arguments.verifier, root, output)
        require(result.returncode == 1, "unexpected i386 PE was accepted")
        require(
            "unexpected i386 PE payload" in result.stderr,
            f"wrong unexpected-i386 error: {result.stderr}",
        )
        rogue_i386.unlink()

        write_arm64ec_pe(hybrid, metadata_version=1)
        result = run_verifier(arguments.verifier, root, output)
        require(result.returncode == 1, "invalid CHPE metadata was accepted")
        require(
            "unexpected AMD64 PE payload" in result.stderr,
            f"wrong invalid-CHPE error: {result.stderr}",
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"{Path(sys.argv[0]).name}: {error}", file=sys.stderr)
        raise SystemExit(1) from error
