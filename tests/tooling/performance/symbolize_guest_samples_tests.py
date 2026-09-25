#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Contract of the offline guest-sample symbolizer on a synthetic ELF."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
import pathlib
import struct
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = (pathlib.Path(__file__).resolve().parents[2] /
          "perf/symbolize_guest_samples.py")
SPEC = importlib.util.spec_from_file_location("symbolize_guest_samples", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
symbolize = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = symbolize  # dataclasses resolve their module by name
SPEC.loader.exec_module(symbolize)

TEXT_VADDR = 0x10000
# Two functions of eight instructions each, distinct words throughout.
FIRST = [0x91000400 + i for i in range(8)]
SECOND = [0xD1000400 + i for i in range(8)]


def build_elf() -> bytes:
    text = struct.pack(f"<{len(FIRST + SECOND)}I", *(FIRST + SECOND))
    strtab = b"\0first\0second\0$x\0"
    symbols = b"".join([
        struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0),
        struct.pack("<IBBHQQ", 1, 0x12, 0, 1, TEXT_VADDR, 32),
        struct.pack("<IBBHQQ", 7, 0x12, 0, 1, TEXT_VADDR + 32, 32),
        struct.pack("<IBBHQQ", 14, 0, 0, 1, TEXT_VADDR, 0),
    ])
    header_size, phdr_size, shdr_size = 64, 56, 64
    text_offset = header_size + phdr_size
    symtab_offset = text_offset + len(text)
    strtab_offset = symtab_offset + len(symbols)
    shoff = strtab_offset + len(strtab)
    header = b"\x7fELF" + bytes([2, 1, 1]) + bytes(9) + struct.pack(
        "<HHIQQQIHHHHHH", 3, 183, 1, TEXT_VADDR, header_size, shoff, 0,
        header_size, phdr_size, 1, shdr_size, 4, 3)
    phdr = struct.pack("<IIQQQQQQ", 1, 5, text_offset, TEXT_VADDR,
                       TEXT_VADDR, len(text), len(text), 0x1000)
    sections = b"".join([
        struct.pack("<IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        struct.pack("<IIQQQQIIQQ", 0, 1, 6, TEXT_VADDR, text_offset,
                    len(text), 0, 0, 4, 0),
        struct.pack("<IIQQQQIIQQ", 0, 2, 0, 0, symtab_offset, len(symbols),
                    3, 1, 8, 24),
        struct.pack("<IIQQQQIIQQ", 0, 3, 0, 0, strtab_offset, len(strtab),
                    0, 0, 1, 0),
    ])
    return header + phdr + text + symbols + strtab + sections


def words(values: list[int]) -> str:
    return ",".join(f"{value:08x}" for value in values)


class SymbolizeGuestSamplesTests(unittest.TestCase):
    def test_blocks_are_placed_by_their_words_not_their_addresses(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "libdemo.so").write_bytes(build_elf())
            samples = root / "samples.tsv"
            # Guest addresses carry an arbitrary load bias; the third block's
            # words exist in no file.
            samples.write_text(
                "# guest code samples\tpasses 10\tsamples 10\tguest 10\n"
                f"guest\t6\t0x7f0000010010\t4\t{words(FIRST[4:8])}\n"
                f"guest\t3\t0x7f0000010020\t8\t{words(SECOND)}\n"
                f"guest\t1\t0x400000\t4\t{words([1, 2, 3, 4])}\n"
                "host\t5\tdarwin_idle\n")
            profile = symbolize.read_profile(samples)
            elves = [symbolize.read_elf(path)
                     for path in symbolize.elf_files([root])]
            located = symbolize.locate(profile.blocks, elves, 4)
            self.assertEqual(located[0][1], TEXT_VADDR + 16)
            self.assertEqual(located[1][1], TEXT_VADDR + 32)
            self.assertNotIn(2, located)
            self.assertEqual(symbolize.symbol_for(elves[0], TEXT_VADDR + 16),
                             "first+0x10")
            self.assertEqual(symbolize.symbol_for(elves[0], TEXT_VADDR + 36),
                             "second+0x4")

            output = io.StringIO()
            with contextlib.redirect_stdout(output), mock.patch.object(
                    sys, "argv", ["symbolize", str(samples), str(root)]):
                self.assertEqual(symbolize.main(), 0)
            report = output.getvalue()
            self.assertIn("located in 1 ELF files: 90.0%", report)
            self.assertIn("60.00%  first  [libdemo.so]", report)
            self.assertIn("30.00%  second  [libdemo.so]", report)
            self.assertIn("10.00%  0x400000  [[unmatched]]", report)

    def test_short_blocks_are_not_guessed(self) -> None:
        block = symbolize.Block(1, 0x1000, 2, tuple(FIRST[:2]))
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "libdemo.so"
            path.write_bytes(build_elf())
            elf = symbolize.read_elf(path)
            self.assertEqual(symbolize.locate([block], [elf], 4), {})


if __name__ == "__main__":
    unittest.main()
