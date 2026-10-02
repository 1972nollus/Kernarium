#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the arm64 gadget region's pinned layout in linked Mach-O images.

darwin/cpu/arm64/gadgets/engine.S emits the region as parts in their own
sections, and UML::GadgetRegionLayout merges them into __TEXT,__text ahead of
everything else. In every image given this checks that:

- __text starts on a 16 KiB image offset and the parts are its first bytes,
  in the order engine.S and gadget_region.order name, each on its boundary
  with its phase pad and directly after the previous part;
- no part holds a zero word between its bounds, so the phase pads are the
  only unreachable bytes in the region; and
- every PC-relative reference in the code parts stays inside the region, so a
  call to a helper outside it goes through its veneer in the island, whose
  words are all branches.

usage: test_gadget_region_layout.py IMAGE...
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
N_STAB = 0xE0
HOST_PAGE = 16384
GADGETS = Path(__file__).resolve().parents[3] / "darwin/cpu/arm64/gadgets"
# The bound symbols of each part, by its gadget_region_start name.
BOUNDS = {
    "bank": "uml_arm64_gadget_bank",
    "forward": "uml_arm64_gadget_forward_section",
    "vector_forward": "uml_arm64_gadget_vector_forward_section",
    "cold": "uml_arm64_gadget_cold_section",
    "afp": "uml_arm64_gadget_afp_section",
    "veneer": "uml_arm64_gadget_veneer_section",
}
ISLAND = "veneer"


@dataclass(frozen=True)
class Part:
    name: str
    alignment: int
    phase: int


@dataclass(frozen=True)
class Section:
    address: int
    size: int
    offset: int
    alignment: int


def engine_parts(engine: Path) -> list[Part]:
    """The parts in emission order, with their alignment and phase."""
    text = engine.read_text()
    defines = {name: int(value) for name, value in re.findall(
        r"^#define (GADGET_PHASE_\w+) (\d+)$", text, re.M)}
    body = text.split(".macro gadget_bank_bounds", 1)[1].split(".endm", 1)[0]
    parts = []
    for name, alignment, phase in re.findall(
            r"gadget_region_start (\w+), (\d+), (\w+)", body):
        parts.append(Part(name, int(alignment),
                          int(phase) if phase.isdigit() else defines[phase]))
    if not parts or parts[0].name != "bank" or parts[-1].name != ISLAND:
        raise ValueError(f"{engine}: cannot read the region parts")
    return parts


def order_parts(order: Path) -> list[str]:
    names = []
    for line in order.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        prefix = "_uml_arm64_gadget_region_"
        if not line.startswith(prefix):
            raise ValueError(f"{order}: unexpected entry {line}")
        names.append(line[len(prefix):])
    return names


class Image:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        magic, _, _, _, ncmds = struct.unpack_from("<IiiII", self.data, 0)
        if magic != MH_MAGIC_64:
            raise ValueError(f"{path}: not a thin 64-bit Mach-O image")
        self.sections: dict[tuple[str, str], Section] = {}
        self.text_segment = None
        self.symbols: dict[str, int] = {}
        offset = 32
        for _ in range(ncmds):
            command, size = struct.unpack_from("<II", self.data, offset)
            if command == LC_SEGMENT_64:
                self._segment(offset)
            elif command == LC_SYMTAB:
                self._symbols(*struct.unpack_from("<IIII", self.data,
                                                  offset + 8))
            offset += size

    def _segment(self, offset: int) -> None:
        segment = self.data[offset + 8:offset + 24].rstrip(b"\0").decode()
        address = struct.unpack_from("<Q", self.data, offset + 24)[0]
        if segment == "__TEXT":
            self.text_segment = address
        count = struct.unpack_from("<I", self.data, offset + 64)[0]
        entry = offset + 72
        for _ in range(count):
            name = self.data[entry:entry + 16].rstrip(b"\0").decode()
            owner = self.data[entry + 16:entry + 32].rstrip(b"\0").decode()
            address, size, file_offset, alignment = struct.unpack_from(
                "<QQII", self.data, entry + 32)
            self.sections[(owner, name)] = Section(address, size, file_offset,
                                                   alignment)
            entry += 80

    def _symbols(self, symbol_offset: int, count: int, string_offset: int,
                 _string_size: int) -> None:
        for index in range(count):
            string, kind, _, _, value = struct.unpack_from(
                "<IBBHQ", self.data, symbol_offset + 16 * index)
            if kind & N_STAB:
                continue
            end = self.data.index(b"\0", string_offset + string)
            name = self.data[string_offset + string:end].decode()
            self.symbols.setdefault(name, value)

    def symbol(self, name: str) -> int:
        try:
            return self.symbols["_" + name]
        except KeyError:
            raise ValueError(f"{self.path}: no symbol {name}") from None

    def words(self, section: Section, begin: int, end: int) -> tuple[int, ...]:
        start = section.offset + begin - section.address
        return struct.unpack_from(f"<{(end - begin) // 4}I", self.data, start)


def sign(value: int, bits: int) -> int:
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def target(word: int, pc: int) -> int | None:
    """The address a PC-relative instruction refers to, else None."""
    if word & 0x7C000000 == 0x14000000:  # B, BL
        return pc + 4 * sign(word & 0x3FFFFFF, 26)
    if word & 0xFF000000 == 0x54000000:  # B.cond, BC.cond
        return pc + 4 * sign((word >> 5) & 0x7FFFF, 19)
    if word & 0x7E000000 in (0x34000000, 0x36000000):  # CBZ/CBNZ, TBZ/TBNZ
        if word & 0x02000000:
            return pc + 4 * sign((word >> 5) & 0x3FFF, 14)
        return pc + 4 * sign((word >> 5) & 0x7FFFF, 19)
    if word & 0x1F000000 == 0x10000000:  # ADR, ADRP
        immediate = sign(((word >> 5) & 0x7FFFF) << 2 | (word >> 29) & 3, 21)
        if word & 0x80000000:
            return (pc & ~0xFFF) + (immediate << 12)
        return pc + immediate
    if word & 0x3B000000 == 0x18000000:  # LDR, LDRSW, PRFM (literal)
        return pc + 4 * sign((word >> 5) & 0x7FFFF, 19)
    return None


def check(image: Image, parts: list[Part]) -> list[str]:
    errors = []
    text = image.sections.get(("__TEXT", "__text"))
    if text is None or image.text_segment is None:
        return [f"{image.path}: no __TEXT,__text"]
    if text.alignment < 14 or (text.address - image.text_segment) % HOST_PAGE:
        errors.append(f"__text is at image offset "
                      f"0x{text.address - image.text_segment:x} with "
                      f"alignment 2^{text.alignment}, not on a 16 KiB page")
    region = image.symbol("uml_arm64_gadget_region_bank")
    if region != text.address:
        errors.append(f"the region starts at 0x{region:x}, not at the start "
                      f"of __text 0x{text.address:x}")
    previous_end = region
    spans = []
    for part in parts:
        start = image.symbol(f"uml_arm64_gadget_region_{part.name}")
        begin = image.symbol(f"{BOUNDS[part.name]}_begin")
        end = image.symbol(f"{BOUNDS[part.name]}_end")
        boundary = 1 << part.alignment
        if start % boundary:
            errors.append(f"{part.name} starts at 0x{start:x}, off its "
                          f"2^{part.alignment} boundary")
        if not previous_end <= start < previous_end + boundary:
            errors.append(f"{part.name} starts at 0x{start:x}, not directly "
                          f"after the previous part's end 0x{previous_end:x}")
        if begin != start + part.phase:
            errors.append(f"{part.name} entries begin at 0x{begin:x}, not at "
                          f"its start 0x{start:x} plus phase {part.phase}")
        if end < begin or end > text.address + text.size:
            errors.append(f"{part.name} ends at 0x{end:x}, outside __text")
            return errors
        spans.append((part, begin, end))
        previous_end = end
    region_end = previous_end
    for part, begin, end in spans:
        words = image.words(text, begin, end)
        zeros = [begin + 4 * index for index, word in enumerate(words)
                 if word == 0]
        if zeros:
            errors.append(f"{part.name} holds {len(zeros)} zero words, the "
                          f"first at 0x{zeros[0]:x}")
        for index, word in enumerate(words):
            pc = begin + 4 * index
            if part.name == ISLAND:
                if word & 0xFC000000 != 0x14000000:
                    errors.append(f"island word 0x{word:08x} at 0x{pc:x} is "
                                  f"not a branch")
                continue
            destination = target(word, pc)
            if destination is not None and not region <= destination < region_end:
                errors.append(f"{part.name} word 0x{word:08x} at 0x{pc:x} "
                              f"refers to 0x{destination:x} outside the "
                              f"region")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--engine", type=Path, default=GADGETS / "engine.S")
    parser.add_argument("--order", type=Path,
                        default=GADGETS / "gadget_region.order")
    parser.add_argument("images", type=Path, nargs="+")
    arguments = parser.parse_args()
    parts = engine_parts(arguments.engine)
    order = order_parts(arguments.order)
    failed = False
    if order != [part.name for part in parts]:
        print(f"{arguments.order} names {order}, but engine.S emits "
              f"{[part.name for part in parts]}", file=sys.stderr)
        failed = True
    for path in arguments.images:
        try:
            image = Image(path)
            errors = check(image, parts)
        except ValueError as error:
            print(error, file=sys.stderr)
            failed = True
            continue
        for error in errors[:20]:
            print(f"{path}: {error}", file=sys.stderr)
        if len(errors) > 20:
            print(f"{path}: {len(errors) - 20} more errors", file=sys.stderr)
        if errors:
            failed = True
            continue
        region = image.symbol("uml_arm64_gadget_region_bank")
        end = image.symbol(f"{BOUNDS[ISLAND]}_end")
        print(f"{path}: region at image offset "
              f"0x{region - image.text_segment:x}, {end - region} bytes, "
              f"phases {[part.phase for part in parts]}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
