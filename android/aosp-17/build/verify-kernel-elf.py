#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import struct
from pathlib import Path


ET_DYN = 3
EM_AARCH64 = 183
PT_LOAD = 1
PT_GNU_STACK = 0x6474E551
PF_X = 1
PF_W = 2
PAGE_SIZE = 16384


def fail(message: str) -> None:
    raise SystemExit(f"Android kernel ELF verifier: {message}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("kernel", type=Path)
    args = parser.parse_args()
    payload = args.kernel.read_bytes()

    if len(payload) < 64 or payload[:4] != b"\x7fELF":
        fail("input is not an ELF image")
    if payload[4] != 2 or payload[5] != 1:
        fail("kernel must be a little-endian ELF64 image")
    elf_type, machine = struct.unpack_from("<HH", payload, 16)
    if elf_type != ET_DYN:
        fail(f"kernel type is {elf_type}, expected ET_DYN")
    if machine != EM_AARCH64:
        fail(f"kernel machine is {machine}, expected AArch64")

    program_offset = struct.unpack_from("<Q", payload, 32)[0]
    entry_size, entry_count = struct.unpack_from("<HH", payload, 54)
    if entry_size < 56 or program_offset + entry_size * entry_count > len(payload):
        fail("program-header table is malformed")

    load_count = 0
    has_executable = False
    has_writable = False
    for index in range(entry_count):
        offset = program_offset + index * entry_size
        (kind, flags, file_offset, virtual_address, _physical_address,
         file_size, memory_size, alignment) = struct.unpack_from(
            "<IIQQQQQQ", payload, offset
        )
        if file_size > memory_size:
            fail(f"program header {index} has file size above memory size")
        if kind == PT_GNU_STACK and flags & PF_X:
            fail("GNU stack is executable")
        if kind != PT_LOAD:
            continue
        load_count += 1
        if flags & PF_W and flags & PF_X:
            fail(f"load segment {index} is writable and executable")
        if alignment < PAGE_SIZE or alignment & (alignment - 1):
            fail(f"load segment {index} has invalid alignment {alignment:#x}")
        if virtual_address % alignment != file_offset % alignment:
            fail(f"load segment {index} violates ELF alignment congruence")
        has_executable |= bool(flags & PF_X)
        has_writable |= bool(flags & PF_W)

    if load_count < 2 or not has_executable or not has_writable:
        fail("kernel lacks the expected separated executable and writable loads")
    print("verified 16 KiB AArch64 ET_DYN Android kernel ELF")


if __name__ == "__main__":
    main()
