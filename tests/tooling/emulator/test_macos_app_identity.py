#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that a macOS emulator executable carries its bundle identifier.

Metal keeps its shader cache below the process's bundle identifier, and
CoreFoundation reads the identifier of a command-line process from the
``__TEXT,__info_plist`` section. This reads that section from the built
executable and compares it with the identifier the build configured.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from pathlib import Path
import plistlib
import re
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
LC_SEGMENT_64 = 0x19
# Reverse-DNS form of CFBundleIdentifier: letters, digits, hyphens and dots.
IDENTIFIER = re.compile(r"[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+")


def thin_slices(data: bytes):
    magic = struct.unpack_from(">I", data)[0]
    if magic != FAT_MAGIC:
        yield data
        return
    count = struct.unpack_from(">I", data, 4)[0]
    for index in range(count):
        _, _, offset, size, _ = struct.unpack_from(">iiIII", data, 8 + 20 * index)
        yield data[offset : offset + size]


def section(slice_: bytes, segment: str, name: str) -> bytes | None:
    if struct.unpack_from("<I", slice_)[0] != MH_MAGIC_64:
        raise SystemExit("not a 64-bit little-endian Mach-O slice")
    commands = struct.unpack_from("<I", slice_, 16)[0]
    offset = 32
    for _ in range(commands):
        command, size = struct.unpack_from("<II", slice_, offset)
        if command == LC_SEGMENT_64:
            sections = struct.unpack_from("<I", slice_, offset + 64)[0]
            for index in range(sections):
                base = offset + 72 + 80 * index
                sect = slice_[base : base + 16].rstrip(b"\0").decode()
                seg = slice_[base + 16 : base + 32].rstrip(b"\0").decode()
                if (seg, sect) == (segment, name):
                    length, position = struct.unpack_from("<QI", slice_, base + 40)
                    return slice_[position : position + length]
        offset += size
    return None


def check(executable: Path, identifier: str, name: str) -> list[str]:
    problems = []
    if not IDENTIFIER.fullmatch(identifier):
        problems.append(f"configured identifier {identifier!r} is not reverse-DNS")
    slices = list(thin_slices(executable.read_bytes()))
    if not slices:
        return problems + [f"{executable}: no Mach-O slice"]
    for number, slice_ in enumerate(slices):
        text = section(slice_, "__TEXT", "__info_plist")
        if text is None:
            problems.append(f"{executable}: slice {number} has no __TEXT,__info_plist")
            continue
        try:
            properties = plistlib.loads(text)
        except Exception as error:  # plistlib raises several types
            problems.append(f"{executable}: slice {number} plist is invalid: {error}")
            continue
        if properties.get("CFBundleIdentifier") != identifier:
            problems.append(
                f"{executable}: CFBundleIdentifier is "
                f"{properties.get('CFBundleIdentifier')!r}, not {identifier!r}"
            )
        if properties.get("CFBundleName") != name:
            problems.append(
                f"{executable}: CFBundleName is "
                f"{properties.get('CFBundleName')!r}, not {name!r}"
            )
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--identifier", required=True)
    parser.add_argument(
        "--executable",
        action="append",
        required=True,
        type=Path,
        help="a built executable; its file name is the expected CFBundleName",
    )
    arguments = parser.parse_args()
    problems = []
    for executable in arguments.executable:
        problems += check(executable, arguments.identifier, executable.name)
    for problem in problems:
        print(problem, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
