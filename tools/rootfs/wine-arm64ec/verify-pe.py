#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Inventory PE images and enforce the ARM64EC Wine appliance boundary."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
from pathlib import Path
import stat
import sys
from typing import BinaryIO


MACHINES = {
    0x014C: "I386",
    0x8664: "AMD64",
    0xA641: "ARM64EC",
    0xA64E: "ARM64X",
    0xAA64: "ARM64",
}
AMD64_SMOKE_PATH = "opt/uml-wine-smoke/x86_64-exit-42.exe"
I386_SMOKE_PATH = "opt/uml-wine-smoke/i386-exit-43.exe"
I386_WINE_PREFIX = "opt/wine-arm64ec/lib/wine/i386-windows/"
AMD64_MACHINE = 0x8664
ARM64EC_MACHINE = 0xA641
PE32_PLUS_MAGIC = 0x20B
COFF_HEADER_SIZE = 20
SECTION_HEADER_SIZE = 40
DATA_DIRECTORY_OFFSET = 112
LOAD_CONFIG_DIRECTORY_INDEX = 10
LOAD_CONFIG_CHPE_POINTER_OFFSET = 0xC8
LOAD_CONFIG_CHPE_POINTER_END = 0xD0
ARM64EC_METADATA_VERSION = 2


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def read_at(
    stream: BinaryIO, file_size: int, offset: int, size: int
) -> bytes | None:
    if offset < 0 or size < 0 or offset > file_size - size:
        return None
    stream.seek(offset)
    data = stream.read(size)
    return data if len(data) == size else None


def rva_to_file_offset(
    rva: int,
    required_size: int,
    file_size: int,
    size_of_headers: int,
    section_table: bytes,
) -> int | None:
    if rva < size_of_headers and required_size <= size_of_headers - rva:
        if required_size <= file_size - rva:
            return rva
    for offset in range(0, len(section_table), SECTION_HEADER_SIZE):
        section = section_table[offset : offset + SECTION_HEADER_SIZE]
        virtual_address = int.from_bytes(section[12:16], "little")
        raw_size = int.from_bytes(section[16:20], "little")
        raw_offset = int.from_bytes(section[20:24], "little")
        if rva < virtual_address:
            continue
        delta = rva - virtual_address
        if delta > raw_size or required_size > raw_size - delta:
            continue
        result = raw_offset + delta
        if result <= file_size and required_size <= file_size - result:
            return result
    return None


def arm64ec_machine(
    stream: BinaryIO,
    file_size: int,
    pe_offset: int,
    coff_header: bytes,
) -> int | None:
    section_count = int.from_bytes(coff_header[2:4], "little")
    optional_size = int.from_bytes(coff_header[16:18], "little")
    load_config_directory_offset = (
        DATA_DIRECTORY_OFFSET + LOAD_CONFIG_DIRECTORY_INDEX * 8
    )
    if optional_size < load_config_directory_offset + 8:
        return None
    optional_offset = pe_offset + 4 + COFF_HEADER_SIZE
    optional_header = read_at(stream, file_size, optional_offset, optional_size)
    if optional_header is None:
        return None
    if int.from_bytes(optional_header[:2], "little") != PE32_PLUS_MAGIC:
        return None
    directory_count = int.from_bytes(optional_header[108:112], "little")
    if directory_count <= LOAD_CONFIG_DIRECTORY_INDEX:
        return None

    directory = optional_header[
        load_config_directory_offset : load_config_directory_offset + 8
    ]
    load_config_rva = int.from_bytes(directory[:4], "little")
    load_config_size = int.from_bytes(directory[4:8], "little")
    if not load_config_rva or load_config_size < LOAD_CONFIG_CHPE_POINTER_END:
        return None

    section_table_offset = optional_offset + optional_size
    section_table = read_at(
        stream,
        file_size,
        section_table_offset,
        section_count * SECTION_HEADER_SIZE,
    )
    if section_table is None:
        return None
    size_of_headers = int.from_bytes(optional_header[60:64], "little")
    load_config_offset = rva_to_file_offset(
        load_config_rva,
        LOAD_CONFIG_CHPE_POINTER_END,
        file_size,
        size_of_headers,
        section_table,
    )
    if load_config_offset is None:
        return None
    load_config = read_at(
        stream, file_size, load_config_offset, LOAD_CONFIG_CHPE_POINTER_END
    )
    if load_config is None:
        return None
    if int.from_bytes(load_config[:4], "little") < LOAD_CONFIG_CHPE_POINTER_END:
        return None

    image_base = int.from_bytes(optional_header[24:32], "little")
    image_size = int.from_bytes(optional_header[56:60], "little")
    metadata_pointer = int.from_bytes(
        load_config[
            LOAD_CONFIG_CHPE_POINTER_OFFSET:LOAD_CONFIG_CHPE_POINTER_END
        ],
        "little",
    )
    if metadata_pointer < image_base:
        return None
    metadata_rva = metadata_pointer - image_base
    if metadata_rva > image_size or 4 > image_size - metadata_rva:
        return None
    metadata_offset = rva_to_file_offset(
        metadata_rva, 4, file_size, size_of_headers, section_table
    )
    if metadata_offset is None:
        return None
    metadata_version = read_at(stream, file_size, metadata_offset, 4)
    if metadata_version is None:
        return None
    if int.from_bytes(metadata_version, "little") != ARM64EC_METADATA_VERSION:
        return None
    return ARM64EC_MACHINE


def pe_machine(path: Path) -> int | None:
    file_size = path.stat().st_size
    with path.open("rb") as stream:
        dos = read_at(stream, file_size, 0, 64)
        if dos is None or dos[:2] != b"MZ":
            return None
        pe_offset = int.from_bytes(dos[60:64], "little")
        if pe_offset < 64:
            return None
        header = read_at(
            stream, file_size, pe_offset, 4 + COFF_HEADER_SIZE
        )
        if header is None or header[:4] != b"PE\0\0":
            return None
        coff_header = header[4:]
        machine = int.from_bytes(coff_header[:2], "little")
        if machine == AMD64_MACHINE:
            return arm64ec_machine(
                stream, file_size, pe_offset, coff_header
            ) or machine
        return machine


def inventory(root: Path) -> list[tuple[str, str]]:
    entries: list[tuple[str, str]] = []
    arm64ec_bridge = False
    wow64_bridge = False
    amd64_smoke = False
    i386_smoke = False
    i386_runtime = False
    native = False
    for directory, _, filenames in os.walk(root, followlinks=False):
        for filename in filenames:
            path = Path(directory, filename)
            try:
                mode = path.stat(follow_symlinks=False).st_mode
            except FileNotFoundError:
                continue
            if not stat.S_ISREG(mode):
                continue
            machine_number = pe_machine(path)
            if machine_number is None:
                continue
            relative = str(path.relative_to(root))
            if "\t" in relative or "\n" in relative:
                raise RuntimeError(f"PE path is not TSV-safe: {relative!r}")
            try:
                machine = MACHINES[machine_number]
            except KeyError as error:
                raise RuntimeError(
                    f"unsupported PE machine: {relative}: 0x{machine_number:04x}"
                ) from error
            if machine == "AMD64" and relative != AMD64_SMOKE_PATH:
                raise RuntimeError(f"unexpected AMD64 PE payload: {relative}")
            if machine == "I386" and not (
                relative == I386_SMOKE_PATH
                or relative.startswith(I386_WINE_PREFIX)
            ):
                raise RuntimeError(f"unexpected i386 PE payload: {relative}")
            if relative == AMD64_SMOKE_PATH:
                if machine != "AMD64":
                    raise RuntimeError("the AMD64 smoke payload is not AMD64")
                amd64_smoke = True
            if relative == I386_SMOKE_PATH:
                if machine != "I386":
                    raise RuntimeError("the i386 smoke payload is not i386")
                i386_smoke = True
            if machine == "I386" and relative.startswith(I386_WINE_PREFIX):
                i386_runtime = True
            if filename.casefold() == "xtajit64.dll":
                if machine != "ARM64EC":
                    raise RuntimeError(f"xtajit64 bridge is not ARM64EC: {relative}")
                arm64ec_bridge = True
            if filename.casefold() == "xtajit.dll":
                if machine != "ARM64":
                    raise RuntimeError(f"xtajit WoW64 bridge is not ARM64: {relative}")
                wow64_bridge = True
            if machine in {"ARM64", "ARM64EC", "ARM64X"}:
                native = True
            entries.append((relative, machine))
    if not amd64_smoke:
        raise RuntimeError(f"missing AMD64 smoke payload: {AMD64_SMOKE_PATH}")
    if not i386_smoke:
        raise RuntimeError(f"missing i386 smoke payload: {I386_SMOKE_PATH}")
    if not i386_runtime:
        raise RuntimeError(f"missing i386 Wine PE runtime under {I386_WINE_PREFIX}")
    if not arm64ec_bridge:
        raise RuntimeError("missing ARM64EC xtajit64.dll bridge")
    if not wow64_bridge:
        raise RuntimeError("missing ARM64 xtajit.dll WoW64 bridge")
    if not native:
        raise RuntimeError("root contains no native ARM64/ARM64EC/ARM64X PE images")
    return sorted(entries)


def main() -> int:
    options = parse_arguments()
    root = options.root.resolve(strict=True)
    if not root.is_dir():
        raise RuntimeError(f"root is not a directory: {root}")
    output = options.output.resolve(strict=False)
    if output.is_symlink():
        raise RuntimeError(f"refusing symlink output: {output}")
    entries = inventory(root)
    with output.open("w", encoding="utf-8", newline="\n") as stream:
        for relative, machine in entries:
            stream.write(f"{relative}\t{machine}\n")
    print(
        f"Verified {len(entries)} Wine PE payloads and both x86 smoke images"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"{Path(sys.argv[0]).name}: {error}", file=sys.stderr)
        raise SystemExit(1) from error
