#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the x86-64 gadget region's pinned layout in linked Mach-O images.

darwin/cpu/x86_64/gadgets/engine/region.inc emits the region as a bank and a
cold part in sections of their own, each a run of slots from its table, and
UML::GadgetRegionLayout merges them into __TEXT,__text ahead of all code but
the arm64 gadget region. In every image given this checks that:

- the bank starts __text, or in an image that also carries the arm64 region
  the first 16 KiB page after it, and the cold part starts on the first page
  after the bank's slots, with only zero bytes before each;
- each group's code begins at its slot's start plus its phase and ends inside
  its slot with an unconditional branch, the bytes around it are zero and no
  word inside it is, so the gaps the table designs are the only unreachable
  bytes in the region;
- every PC-relative reference in the region stays inside it, but the island's
  words, which are all branches; and
- the order file names the region's parts after the arm64 region's.

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
LINE = 128
CPU = Path(__file__).resolve().parents[3] / "darwin/cpu"
BANK = "uml_x86_64_gadget_region_bank"
COLD = "uml_x86_64_gadget_cold_section_begin"
# Every arm64 region part's end label; the last part's ends the region.
ARM64_END = re.compile(r"_uml_arm64_gadget_\w+_section_end")
ISLAND = "veneer"
SHOWN = 20


@dataclass(frozen=True)
class Slot:
    name: str
    bank_phase: int
    bank_slot: int
    cold_phase: int
    cold_slot: int


@dataclass(frozen=True)
class Section:
    address: int
    size: int
    offset: int


def table(region: Path) -> list[Slot]:
    """The slot table, in its order."""
    slots = [Slot(name, *(int(value, 0) for value in values))
             for name, *values in re.findall(
                 r"^\s*gadget_slot (\w+),\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+)\s*$",
                 region.read_text(), re.M)]
    names = [slot.name for slot in slots]
    if not slots or names[0] != "common" or ISLAND not in names or \
            len(set(names)) != len(names):
        raise ValueError(f"{region}: cannot read the slot table")
    for slot in slots:
        if max(slot.bank_phase, slot.cold_phase) >= LINE or \
                slot.bank_slot % LINE or slot.cold_slot % LINE:
            raise ValueError(f"{region}: slot {slot.name} is off its lines")
    return slots


def order_parts(order: Path) -> list[str]:
    """The order file's x86-64 entries, which must follow the others."""
    names = [line.strip() for line in order.read_text().splitlines()
             if line.strip() and not line.startswith("#")]
    ours = [index for index, name in enumerate(names)
            if name.startswith("_uml_x86_64_")]
    if ours and ours != list(range(ours[0], len(names))):
        raise ValueError(f"{order}: the x86-64 parts are not the last entries")
    return [names[index][1:] for index in ours]


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
                self._symbols(*struct.unpack_from("<III", self.data,
                                                  offset + 8))
            offset += size

    def _segment(self, offset: int) -> None:
        segment = self.data[offset + 8:offset + 24].rstrip(b"\0").decode()
        if segment == "__TEXT":
            self.text_segment = struct.unpack_from("<Q", self.data,
                                                   offset + 24)[0]
        count = struct.unpack_from("<I", self.data, offset + 64)[0]
        entry = offset + 72
        for _ in range(count):
            name = self.data[entry:entry + 16].rstrip(b"\0").decode()
            owner = self.data[entry + 16:entry + 32].rstrip(b"\0").decode()
            address, size, file_offset = struct.unpack_from(
                "<QQI", self.data, entry + 32)
            self.sections[(owner, name)] = Section(address, size, file_offset)
            entry += 80

    def _symbols(self, symbol_offset: int, count: int,
                 string_offset: int) -> None:
        for index in range(count):
            string, kind, _, _, value = struct.unpack_from(
                "<IBBHQ", self.data, symbol_offset + 16 * index)
            if kind & N_STAB:
                continue
            end = self.data.index(b"\0", string_offset + string)
            name = self.data[string_offset + string:end].decode()
            self.symbols.setdefault(name, value)

    def symbol(self, name: str) -> int | None:
        return self.symbols.get("_" + name)

    def arm64_end(self) -> int | None:
        """Where the arm64 region ends, if the image carries it."""
        ends = [value for name, value in self.symbols.items()
                if ARM64_END.fullmatch(name)]
        return max(ends) if ends else None

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


def unconditional(word: int) -> bool:
    """B, BR or RET: nothing falls through it."""
    return word & 0xFC000000 == 0x14000000 or \
        word & 0xFFFFFC1F in (0xD61F0000, 0xD65F0000)


def page(address: int) -> int:
    return -(-address // HOST_PAGE) * HOST_PAGE


def check(image: Image, slots: list[Slot]) -> tuple[list[str], str]:
    text = image.sections.get(("__TEXT", "__text"))
    if text is None or image.text_segment is None:
        return [f"{image.path}: no __TEXT,__text"], ""
    errors = []
    bank, cold = image.symbol(BANK), image.symbol(COLD)
    if bank is None or cold is None:
        return [f"no symbol {BANK if bank is None else COLD}"], ""
    arm64_end = image.arm64_end()
    expected = text.address if arm64_end is None else page(arm64_end)
    if bank != expected:
        errors.append(f"the bank starts at 0x{bank:x}, not at 0x{expected:x}"
                      + (" (__text's start)" if arm64_end is None else
                         " (the first page after the arm64 region)"))
    if (bank - image.text_segment) % HOST_PAGE:
        errors.append(f"the bank is at image offset "
                      f"0x{bank - image.text_segment:x}, off a 16 KiB page")
    bank_end = bank + sum(slot.bank_slot for slot in slots)
    if cold != page(bank_end):
        errors.append(f"the cold part starts at 0x{cold:x}, not on the first "
                      f"page 0x{page(bank_end):x} after the bank's slots")
    cold_end = cold + sum(slot.cold_slot for slot in slots)
    if cold_end > text.address + text.size:
        errors.append(f"the cold part ends at 0x{cold_end:x}, outside __text")
    if errors:
        return errors, ""
    region = (bank, cold_end)
    lead = [] if arm64_end is None else [(arm64_end, bank, "before the bank")]
    gaps = [(bank_end, cold, "between the parts")] + lead
    gap_bytes = cold - bank_end
    for begin, end, where in gaps:
        if any(image.words(text, begin, end)):
            errors.append(f"a nonzero word lies {where}, in "
                          f"0x{begin:x}..0x{end:x}")
    for part, start in (("bank", bank), ("cold", cold)):
        for slot in slots:
            phase = getattr(slot, f"{part}_phase")
            size = getattr(slot, f"{part}_slot")
            if size == 0:
                continue
            words = image.words(text, start, start + size)
            where = f"{slot.name}'s {part} slot at 0x{start:x}"
            used = [index for index, word in enumerate(words) if word]
            if not used:
                errors.append(f"{where} holds no code")
                start += size
                gap_bytes += size
                continue
            first, last = 4 * used[0], 4 * used[-1]
            gap_bytes += size - (last + 4 - first)
            if first != phase:
                errors.append(f"{where} begins its code at +{first}, not at "
                              f"its phase {phase}")
            if len(used) != (last - first) // 4 + 1:
                inner = next(4 * index for index in range(used[0], used[-1])
                             if not words[index])
                errors.append(f"{where} holds a zero word at "
                              f"0x{start + inner:x} inside its code")
            if not unconditional(words[last // 4]):
                errors.append(f"{where} ends its code at 0x{start + last:x} "
                              f"with 0x{words[last // 4]:08x}, which falls "
                              f"through into the gap")
            for index in used:
                word, pc = words[index], start + 4 * index
                if slot.name == ISLAND:
                    if word & 0xFC000000 != 0x14000000:
                        errors.append(f"island word 0x{word:08x} at 0x{pc:x} "
                                      f"is not a branch")
                    continue
                destination = target(word, pc)
                if destination is not None and \
                        not region[0] <= destination < region[1]:
                    errors.append(f"{slot.name} word 0x{word:08x} at "
                                  f"0x{pc:x} refers to 0x{destination:x} "
                                  f"outside the region")
            start += size
    summary = (f"x86-64 region at image offset 0x{bank - image.text_segment:x}"
               f", bank {bank_end - bank} and cold part {cold_end - cold} "
               f"bytes in {len(slots)} groups, {gap_bytes} unreachable")
    return errors, summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--region", type=Path,
                        default=CPU / "x86_64/gadgets/engine/region.inc")
    parser.add_argument("--order", type=Path,
                        default=CPU / "gadget_region.order")
    parser.add_argument("images", type=Path, nargs="+")
    arguments = parser.parse_args()
    failed = False
    try:
        slots = table(arguments.region)
        order = order_parts(arguments.order)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1
    if order != [BANK, COLD]:
        print(f"{arguments.order} names {order} for the x86-64 region, not "
              f"{[BANK, COLD]}", file=sys.stderr)
        failed = True
    for path in arguments.images:
        try:
            errors, summary = check(Image(path), slots)
        except ValueError as error:
            print(error, file=sys.stderr)
            failed = True
            continue
        for error in errors[:SHOWN]:
            print(f"{path}: {error}", file=sys.stderr)
        if len(errors) > SHOWN:
            print(f"{path}: {len(errors) - SHOWN} more errors",
                  file=sys.stderr)
        if errors:
            failed = True
            continue
        print(f"{path}: {summary}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
