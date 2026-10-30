#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Author the top-byte-ignore memory cases of the A64 instruction corpus.

usage: tbi-memory-corpus.py DUMP OUTPUT

With top-byte ignore (TBI) the hardware ignores the top byte of a data
address: a pointer with a tag in bits 63:56 reaches the byte its low 56 bits
name, a fault reports that address, and the exclusive monitor, which guards a
location and not a pointer, does not see the tag. Android runs every process
this way (Scudo and bionic's heap tagging), and the virtual CPU does whenever
the host passes --guest-top-byte-ignore (CpuState::tbi_enabled). Without TBI
a tagged pointer is simply an address outside the guest's range and faults at
that address.

The cases go into one arm64-tbi-memory record. Each section below owns one
family of memory forms and its functions are named for it; a section never
reaches into another's:

- atomic read-modify-write (LDADD, LDCLR, LDEOR, LDSET, LDSMAX, LDSMIN,
  LDUMAX, LDUMIN and SWP, in every width and ordering, and the ST forms that
  discard the result);
- compare and swap (CAS) and compare and swap pair (CASP);
- load-acquire and store-release (LDAR, STLR);
- RCpc loads and stores (LDAPR, LDAPUR and its sign-extending forms, STLUR);
- exclusive loads and stores (LDXR, LDAXR, STXR, STLXR and the pair forms),
  and the exclusive monitor across pointers that differ only in the tag.

Every case reaches memory through a tagged base register. The expected
results come from the architecture's pseudocode, written here independently
of the engines: the access completes at the untagged address, the base
register keeps its tag, and a fault reports the untagged address. The
controls run the same tagged base with TBI off, where the access faults at
the tagged address. A misaligned access that crosses a 16-byte block faults
on every host, with or without FEAT_LSE2, so those cases hold everywhere.

The record's memory is one read-write 4 KiB page and one read-only 4 KiB
page at fixed addresses; an access beyond them faults.

Run it on a finished dump, after the SM = 0 family step, and --repack the
result; the 16 KiB retirement and invalid-encoding steps still run last.
"""
from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import dataclasses
import functools
import hashlib
import importlib.util
import json
import pathlib
import re
import sys
import zlib
from typing import Callable

HERE = pathlib.Path(__file__).resolve().parent


def _load(name: str, filename: str):
    module = sys.modules.get(name)
    if module is None:
        spec = importlib.util.spec_from_file_location(name, HERE / filename)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
    return module


corpus = _load("build_unit_corpus", "build-unit-corpus.py")

SITE = "tools/arm64-tests/tbi-memory-corpus.py"
PRODUCER = "arm64-tbi-memory"
SVC = 0xD400_0001
CLREX = 0xD503_3F5F
MASK64 = (1 << 64) - 1
UNTAGGED = 0x00FF_FFFF_FFFF_FFFF

PAGE = 4096
# The read-write page, the read-only page and an address nothing maps.
REGION = 0x5200_0000
READ_ONLY_REGION = 0x5200_2000
UNMAPPED = 0x5200_4040
READ_WRITE_FLAGS = 0x13
READ_ONLY_FLAGS = 0x01
# The accesses start here; the last 256 bytes of the page hold zeros, for the
# compare-with-zero forms.
ACCESS = REGION + 0x100
ZERO_AREA = REGION + 0xF00

# Tag bytes cycle through these. Bit 55 selects the translation table on
# hardware, so no tag sets a bit below bit 56.
TAGS = (0xB4, 0x5A, 0x01, 0xFF, 0x80, 0x2C, 0x7F, 0xA5)

# The bytes at which a misaligned access of each size crosses a 16-byte block.
CROSSING = {2: 15, 4: 13, 8: 12, 16: 8}

X_SOURCE = 0x8123_4567_89AB_CDEF
X_OTHER = 0x7766_5544_3322_1100

SIZE_NAMES = {0: "b", 1: "h", 2: "w", 3: "x"}


# A size is the log2 of the bytes an access moves: 0 byte, 1 halfword, 2 word,
# 3 doubleword.
def size_bytes(size: int) -> int:
    return 1 << size


def mask(size: int) -> int:
    return (1 << (8 << size)) - 1


def signed(value: int, size: int) -> int:
    value &= mask(size)
    return value - (1 << (8 << size)) if value >> ((8 << size) - 1) else value


@functools.cache
def page_blob() -> bytes:
    """Bytes with no short period, so a misplaced access cannot pass, and a
    zero tail."""
    data = bytearray(((index * 0x9D) + 0x3B) & 0xFF for index in range(PAGE))
    data[ZERO_AREA - REGION:] = bytes(PAGE - (ZERO_AREA - REGION))
    return bytes(data)


def register_text(number: int, wide: bool) -> str:
    if number == 31:
        return "xzr" if wide else "wzr"
    return f"{'x' if wide else 'w'}{number}"


def text(number: int, size: int) -> str:
    return register_text(number, size == 3)


@dataclasses.dataclass
class Case:
    """One case: a program, the registers it starts with, and its result.

    `effect(memory, registers)` applies a completing case to the page's bytes
    (the offset of an address in the page is address - REGION) and to a dict
    of the general registers by number, the way the architecture does.
    """

    name: str
    words: list[int]
    native: list[str]
    registers: dict[int, int]
    effect: Callable | None = None
    # (address, access) of the fault a case expects; None completes.
    fault: tuple[int, str] | None = None
    tbi: bool = True
    read_only: bool = False
    retired: object = None

    def __post_init__(self) -> None:
        if self.retired is None:
            self.retired = 0 if self.fault else len(self.words)


def tagged(address: int, index: int) -> int:
    return (TAGS[index % len(TAGS)] << 56) | address


# --------------------------------------------------------------------------
# Encodings. Every word is checked against LLVM MC by the freshness test of
# the pack's authoring, which disassembles the program of each case.


def rmw_word(size: int, acquire: int, release: int, rs: int, operation: int,
             rn: int, rt: int) -> int:
    """LD<op>, ST<op> and SWP: operation is o3 << 3 | opc."""
    return (size << 30 | 0x3800_0000 | acquire << 23 | release << 22 |
            1 << 21 | rs << 16 | operation << 12 | rn << 5 | rt)


def cas_word(size: int, acquire: int, release: int, rs: int, rn: int,
             rt: int) -> int:
    return (size << 30 | 0x0800_0000 | 1 << 23 | acquire << 22 | 1 << 21 |
            rs << 16 | release << 15 | 0x1F << 10 | rn << 5 | rt)


def casp_word(wide: int, acquire: int, release: int, rs: int, rn: int,
              rt: int) -> int:
    return (wide << 30 | 0x0800_0000 | acquire << 22 | 1 << 21 | rs << 16 |
            release << 15 | 0x1F << 10 | rn << 5 | rt)


def ordered_word(size: int, load: bool, rn: int, rt: int) -> int:
    """LDAR and STLR."""
    return (size << 30 | 0x0800_0000 | 1 << 23 | (1 << 22 if load else 0) |
            0x1F << 16 | 1 << 15 | 0x1F << 10 | rn << 5 | rt)


def ldapr_word(size: int, rn: int, rt: int) -> int:
    return (size << 30 | 0x3800_0000 | 1 << 23 | 1 << 21 | 0x1F << 16 |
            0xC000 | rn << 5 | rt)


def ldapur_word(size: int, opc: int, immediate: int, rn: int, rt: int) -> int:
    """LDAPUR, STLUR and the sign-extending loads: opc 0 store, 1 load,
    2 sign-extend to 64 bits, 3 sign-extend to 32."""
    return (size << 30 | 0x1900_0000 | opc << 22 | (immediate & 511) << 12 |
            rn << 5 | rt)


def exclusive_load_word(size: int, acquire: int, rn: int, rt: int) -> int:
    return (size << 30 | 0x0800_0000 | 1 << 22 | 0x1F << 16 | acquire << 15 |
            0x1F << 10 | rn << 5 | rt)


def exclusive_store_word(size: int, release: int, rs: int, rn: int,
                         rt: int) -> int:
    return (size << 30 | 0x0800_0000 | rs << 16 | release << 15 |
            0x1F << 10 | rn << 5 | rt)


def exclusive_load_pair_word(wide: int, acquire: int, rn: int, rt: int,
                             rt2: int) -> int:
    return (0x8000_0000 | wide << 30 | 0x0800_0000 | 1 << 22 | 1 << 21 |
            0x1F << 16 | acquire << 15 | rt2 << 10 | rn << 5 | rt)


def exclusive_store_pair_word(wide: int, release: int, rs: int, rn: int,
                              rt: int, rt2: int) -> int:
    return (0x8000_0000 | wide << 30 | 0x0800_0000 | 1 << 21 | rs << 16 |
            release << 15 | rt2 << 10 | rn << 5 | rt)


def branch_back(words: int, register: int) -> int:
    """CBNZ Wn, <words instructions back>."""
    return 0x3500_0000 | ((-words) & 0x7FFFF) << 5 | register


# --------------------------------------------------------------------------
# Section: atomic read-modify-write


RMW_OPERATIONS = (
    ("add", 0x0), ("clr", 0x1), ("eor", 0x2), ("set", 0x3), ("smax", 0x4),
    ("smin", 0x5), ("umax", 0x6), ("umin", 0x7), ("swp", 0x8),
)
RMW_ORDERS = (("", 0, 0), ("a", 1, 0), ("l", 0, 1), ("al", 1, 1))
# source and result registers: rr, rz (result discarded), zr (source zero), zz
RMW_MODES = ((3, 4), (3, 31), (31, 4), (31, 31))


def rmw_result(name: str, old: int, source: int, size: int) -> int:
    """The value LD<op> stores, from the instruction's pseudocode."""
    source &= mask(size)
    if name == "add":
        new = old + source
    elif name == "clr":
        new = old & ~source
    elif name == "eor":
        new = old ^ source
    elif name == "set":
        new = old | source
    elif name == "smax":
        new = old if signed(old, size) >= signed(source, size) else source
    elif name == "smin":
        new = old if signed(old, size) <= signed(source, size) else source
    elif name == "umax":
        new = max(old, source)
    elif name == "umin":
        new = min(old, source)
    else:
        new = source
    return new & mask(size)


def rmw_mnemonic(name: str, order: str, size: int, discard: bool) -> str:
    if discard and name != "swp" and "a" not in order:
        return f"st{name}{'l' if 'l' in order else ''}{SIZE_NAMES_B[size]}"
    return f"{'swp' if name == 'swp' else 'ld' + name}{order}{SIZE_NAMES_B[size]}"


SIZE_NAMES_B = {0: "b", 1: "h", 2: "", 3: ""}


def rmw_text(name: str, order: str, size: int, rs: int, rt: int) -> str:
    discard = rt == 31
    mnemonic = rmw_mnemonic(name, order, size, discard)
    wide = size == 3
    if discard and mnemonic.startswith("st"):
        return f"{mnemonic} {register_text(rs, wide)}, [x2]"
    return (f"{mnemonic} {register_text(rs, wide)}, "
            f"{register_text(rt, wide)}, [x2]")


def rmw_cases() -> list[Case]:
    cases: list[Case] = []
    index = 0
    for name, operation in RMW_OPERATIONS:
        for order, acquire, release in RMW_ORDERS:
            for size in range(4):
                for rs, rt in RMW_MODES:
                    index += 1
                    byte = size_bytes(size)
                    address = ACCESS + 0x20 + byte * (index % 4)
                    word = rmw_word(size, acquire, release, rs, operation,
                                    2, rt)

                    def effect(memory, registers, name=name, size=size,
                               address=address, rs=rs, rt=rt):
                        offset = address - REGION
                        old = int.from_bytes(memory[offset:offset +
                                             size_bytes(size)], "little")
                        source = 0 if rs == 31 else registers[rs]
                        new = rmw_result(name, old, source, size)
                        memory[offset:offset + size_bytes(size)] = \
                            new.to_bytes(size_bytes(size), "little")
                        if rt != 31:
                            registers[rt] = old

                    tag = TAGS[index % len(TAGS)]
                    cases.append(Case(
                        f"{rmw_text(name, order, size, rs, rt)} "
                        f"tagged 0x{tag:02x}",
                        [word, SVC], ["lse"],
                        {2: tagged(address, index), 3: X_SOURCE},
                        effect))
    # A misaligned access that crosses a 16-byte block faults at the untagged
    # address, as a write; the engine reports it before any byte moves.
    for name, operation in RMW_OPERATIONS:
        for order, acquire, release in RMW_ORDERS:
            for size in (1, 2, 3):
                for rs, rt in RMW_MODES:
                    index += 1
                    address = ACCESS + CROSSING[size_bytes(size)]
                    word = rmw_word(size, acquire, release, rs, operation,
                                    2, rt)
                    tag = TAGS[index % len(TAGS)]
                    cases.append(Case(
                        f"{rmw_text(name, order, size, rs, rt)} tagged "
                        f"0x{tag:02x} across a block",
                        [word, SVC], ["lse"],
                        {2: tagged(address, index), 3: X_SOURCE},
                        fault=(address, "write")))
    return cases


# --------------------------------------------------------------------------
# Section: compare and swap, and compare and swap pair


CAS_ORDERS = (("", 0, 0), ("a", 1, 0), ("l", 0, 1), ("al", 1, 1))
CAS_MODES = ((3, 4), (3, 31), (31, 4), (31, 31))


def cas_text(order: str, size: int, rs: int, rt: int) -> str:
    wide = size == 3
    return (f"cas{order}{SIZE_NAMES_B[size]} {register_text(rs, wide)}, "
            f"{register_text(rt, wide)}, [x2]")


def cas_cases() -> list[Case]:
    cases: list[Case] = []
    index = 0
    for order, acquire, release in CAS_ORDERS:
        for size in range(4):
            for rs, rt in CAS_MODES:
                for match in (True, False):
                    # A compare with zero matches only where memory is zero.
                    if rs == 31 and not match:
                        continue
                    index += 1
                    byte = size_bytes(size)
                    base = ZERO_AREA + 0x40 if rs == 31 else ACCESS + 0x40
                    address = base + byte * (index % 4)
                    word = cas_word(size, acquire, release, rs, 2, rt)
                    registers = {2: tagged(address, index), 4: X_OTHER}
                    memory_old = int.from_bytes(page_blob()[
                        address - REGION:address - REGION + byte], "little")
                    if rs != 31:
                        # The comparison is on the low bytes only, so the
                        # rest of the register holds a pattern.
                        compare = (memory_old if match else
                                   memory_old ^ 0x5A5A_5A5A_5A5A_5A5A) \
                            & mask(size)
                        registers[3] = (0x5EED_0000_0000_0000 | compare
                                        if size < 3 else compare)

                    def effect(memory, registers, size=size, address=address,
                               rs=rs, rt=rt):
                        offset = address - REGION
                        byte = size_bytes(size)
                        old = int.from_bytes(memory[offset:offset + byte],
                                             "little")
                        compare = 0 if rs == 31 else registers[rs] & mask(size)
                        new = 0 if rt == 31 else registers[rt] & mask(size)
                        if old == compare:
                            memory[offset:offset + byte] = \
                                new.to_bytes(byte, "little")
                        if rs != 31:
                            registers[rs] = old

                    tag = TAGS[index % len(TAGS)]
                    outcome = "match" if match else "mismatch"
                    cases.append(Case(
                        f"{cas_text(order, size, rs, rt)} tagged 0x{tag:02x} "
                        f"{outcome}", [word, SVC], ["lse"], registers,
                        effect))
    for order, acquire, release in CAS_ORDERS:
        for size in (1, 2, 3):
            index += 1
            address = ACCESS + CROSSING[size_bytes(size)]
            cases.append(Case(
                f"{cas_text(order, size, 3, 4)} tagged "
                f"0x{TAGS[index % len(TAGS)]:02x} across a block",
                [cas_word(size, acquire, release, 3, 2, 4), SVC], ["lse"],
                {2: tagged(address, index), 3: X_SOURCE, 4: X_OTHER},
                fault=(address, "write")))
    return cases


# The pair forms: Rs and Rt are even; register 30 names the pair X30, XZR.
CASP_MODES = ((4, 6), (4, 30), (30, 6), (30, 30))


def casp_text(order: str, wide: int, rs: int, rt: int) -> str:
    def pair(number: int) -> str:
        return (f"{register_text(number, bool(wide))}, "
                f"{register_text(number + 1, bool(wide))}")
    return f"casp{order} {pair(rs)}, {pair(rt)}, [x2]"


def casp_cases() -> list[Case]:
    cases: list[Case] = []
    index = 0
    for order, acquire, release in CAS_ORDERS:
        for wide in (0, 1):
            element = 4 << wide
            for rs, rt in CASP_MODES:
                for match in (True, False):
                    # A compare against the pair (X30, XZR) matches only
                    # where the second element is zero.
                    zero = rs == 30
                    if zero and not match:
                        continue
                    index += 1
                    base = ZERO_AREA + 0x40 if zero else ACCESS + 0x40
                    address = base + 2 * element * (index % 3)
                    word = casp_word(wide, acquire, release, rs, 2, rt)
                    old = [int.from_bytes(page_blob()[
                        address - REGION + element * lane:
                        address - REGION + element * (lane + 1)], "little")
                        for lane in range(2)]
                    registers = {2: tagged(address, index),
                                 6: X_OTHER, 7: X_SOURCE}
                    if rs != 30:
                        first, second = old if match else \
                            (old[0] ^ 1, old[1])
                        registers[rs] = 0x5EED_0000_0000_0000 | first \
                            if not wide else first
                        registers[rs + 1] = 0x5EED_0000_0000_0000 | second \
                            if not wide else second
                    else:
                        registers[30] = old[0]

                    def effect(memory, registers, address=address, rs=rs,
                               rt=rt, element=element):
                        offset = address - REGION
                        stored = [int.from_bytes(memory[
                            offset + element * lane:
                            offset + element * (lane + 1)], "little")
                            for lane in range(2)]
                        limit = (1 << 8 * element) - 1
                        compare = [registers[rs] & limit,
                                   0 if rs == 30 else registers[rs + 1] & limit]
                        new = [registers[rt] & limit,
                               0 if rt == 30 else registers[rt + 1] & limit]
                        if stored == compare:
                            for lane in range(2):
                                memory[offset + element * lane:
                                       offset + element * (lane + 1)] = \
                                    new[lane].to_bytes(element, "little")
                        registers[rs] = stored[0]
                        if rs != 30:
                            registers[rs + 1] = stored[1]

                    tag = TAGS[index % len(TAGS)]
                    outcome = "match" if match else "mismatch"
                    cases.append(Case(
                        f"{casp_text(order, wide, rs, rt)} tagged "
                        f"0x{tag:02x} {outcome}", [word, SVC], ["lse"],
                        registers, effect))
    for order, acquire, release in CAS_ORDERS:
        for wide in (0, 1):
            index += 1
            total = 8 << wide
            address = ACCESS + (CROSSING[total] if wide == 0 else 8)
            cases.append(Case(
                f"{casp_text(order, wide, 4, 6)} tagged "
                f"0x{TAGS[index % len(TAGS)]:02x} across a block",
                [casp_word(wide, acquire, release, 4, 2, 6), SVC], ["lse"],
                {2: tagged(address, index), 4: X_SOURCE, 5: X_OTHER,
                 6: X_SOURCE, 7: X_OTHER}, fault=(address, "write")))
    return cases


# --------------------------------------------------------------------------
# Section: load-acquire and store-release


def ordered_cases() -> list[Case]:
    cases: list[Case] = []
    index = 0
    for load in (True, False):
        for size in range(4):
            for register in (3, 31):
                index += 1
                byte = size_bytes(size)
                address = ACCESS + 0x80 + byte * (index % 4)
                word = ordered_word(size, load, 2, register)
                mnemonic = ("ldar" if load else "stlr") + SIZE_NAMES_B[size]

                def effect(memory, registers, load=load, size=size,
                           address=address, register=register):
                    offset = address - REGION
                    byte = size_bytes(size)
                    if load:
                        if register != 31:
                            registers[register] = int.from_bytes(
                                memory[offset:offset + byte], "little")
                    else:
                        value = 0 if register == 31 else \
                            registers[register] & mask(size)
                        memory[offset:offset + byte] = \
                            value.to_bytes(byte, "little")

                cases.append(Case(
                    f"{mnemonic} {text(register, size)}, [x2] tagged "
                    f"0x{TAGS[index % len(TAGS)]:02x}", [word, SVC], [],
                    {2: tagged(address, index), 3: X_SOURCE}, effect))
    for load in (True, False):
        for size in (1, 2, 3):
            index += 1
            address = ACCESS + CROSSING[size_bytes(size)]
            mnemonic = ("ldar" if load else "stlr") + SIZE_NAMES_B[size]
            cases.append(Case(
                f"{mnemonic} {text(3, size)}, [x2] tagged "
                f"0x{TAGS[index % len(TAGS)]:02x} across a block",
                [ordered_word(size, load, 2, 3), SVC], [],
                {2: tagged(address, index), 3: X_SOURCE},
                fault=(address, "read" if load else "write")))
    return cases


# --------------------------------------------------------------------------
# Section: RCpc loads and stores


# (suffix, size, opc, destination width): the loads LDAPUR can extend.
RCPC_LOADS = (
    ("b", 0, 1, 32), ("h", 1, 1, 32), ("", 2, 1, 32), ("", 3, 1, 64),
    ("sb", 0, 3, 32), ("sb", 0, 2, 64), ("sh", 1, 3, 32), ("sh", 1, 2, 64),
    ("sw", 2, 2, 64),
)


def rcpc_load_effect(address: int, size: int, opc: int, register: int):
    def effect(memory, registers):
        offset = address - REGION
        byte = size_bytes(size)
        value = int.from_bytes(memory[offset:offset + byte], "little")
        if opc == 3:
            value = signed(value, size) & mask(2)
        elif opc == 2:
            value = signed(value, size) & MASK64
        if register != 31:
            registers[register] = value
    return effect


def rcpc_cases() -> list[Case]:
    cases: list[Case] = []
    index = 0
    # LDAPR: no offset, zero-extending, every width, result kept or dropped.
    for size in range(4):
        for register in (4, 31):
            index += 1
            byte = size_bytes(size)
            address = ACCESS + 0xC0 + byte * (index % 4)
            mnemonic = "ldapr" + SIZE_NAMES_B[size]
            cases.append(Case(
                f"{mnemonic} {text(register, size)}, [x2] tagged "
                f"0x{TAGS[index % len(TAGS)]:02x}",
                [ldapr_word(size, 2, register), SVC], ["lrcpc"],
                {2: tagged(address, index)},
                rcpc_load_effect(address, size, 1, register)))
    # LDAPUR and its sign-extending forms, with offsets of both signs.
    for suffix, size, opc, width in RCPC_LOADS:
        for register, immediate in ((4, 9), (4, -7), (31, 3)):
            index += 1
            byte = size_bytes(size)
            address = ACCESS + 0xE0 + byte * (index % 3)
            mnemonic = f"ldapur{suffix}"
            destination = register_text(register, width == 64)
            cases.append(Case(
                f"{mnemonic} {destination}, [x2, #{immediate}] tagged "
                f"0x{TAGS[index % len(TAGS)]:02x}",
                [ldapur_word(size, opc, immediate, 2, register), SVC],
                ["lrcpc2"], {2: tagged(address - immediate, index)},
                rcpc_load_effect(address, size, opc, register)))
    for size in range(4):
        for register, immediate in ((3, 5), (31, -9)):
            index += 1
            byte = size_bytes(size)
            address = ACCESS + 0xF0 - 0x10 + byte * (index % 3)
            mnemonic = "stlur" + SIZE_NAMES_B[size]

            def effect(memory, registers, size=size, address=address,
                       register=register):
                offset = address - REGION
                byte = size_bytes(size)
                value = 0 if register == 31 else \
                    registers[register] & mask(size)
                memory[offset:offset + byte] = value.to_bytes(byte, "little")

            cases.append(Case(
                f"{mnemonic} {text(register, size)}, [x2, #{immediate}] "
                f"tagged 0x{TAGS[index % len(TAGS)]:02x}",
                [ldapur_word(size, 0, immediate, 2, register), SVC],
                ["lrcpc2"],
                {2: tagged(address - immediate, index), 3: X_SOURCE},
                effect))
    for size in (1, 2, 3):
        index += 1
        address = ACCESS + CROSSING[size_bytes(size)]
        cases.append(Case(
            f"ldapr{SIZE_NAMES_B[size]} {text(4, size)}, [x2] tagged "
            f"0x{TAGS[index % len(TAGS)]:02x} across a block",
            [ldapr_word(size, 2, 4), SVC], ["lrcpc"],
            {2: tagged(address, index)}, fault=(address, "read")))
        index += 1
        cases.append(Case(
            f"ldapur{SIZE_NAMES_B[size]} {text(4, size)}, [x2, #6] tagged "
            f"0x{TAGS[index % len(TAGS)]:02x} across a block",
            [ldapur_word(size, 1, 6, 2, 4), SVC], ["lrcpc2"],
            {2: tagged(address - 6, index)}, fault=(address, "read")))
        index += 1
        cases.append(Case(
            f"stlur{SIZE_NAMES_B[size]} {text(3, size)}, [x2, #-2] tagged "
            f"0x{TAGS[index % len(TAGS)]:02x} across a block",
            [ldapur_word(size, 0, -2, 2, 3), SVC], ["lrcpc2"],
            {2: tagged(address + 2, index), 3: X_SOURCE},
            fault=(address, "write")))
    return cases


# --------------------------------------------------------------------------
# Section: exclusive loads and stores and the monitor


EXCLUSIVE_PROGRAM_RETIRED = {"minimum": 5, "step": 3, "maximum": None}


def exclusive_program(load: int, store: int, status: int) -> list[int]:
    """Load, store, retry the pair until the store completes, drop the
    monitor and stop: the shape a compiler emits for an atomic update, and
    the one the FEAT_LSE2 cases use. A store that discards its status cannot
    be retried, and runs once."""
    if status == 31:
        return [load, store, CLREX, SVC]
    return [load, store, branch_back(2, status), CLREX, SVC]


def exclusive_retired(status: int) -> object:
    return 4 if status == 31 else EXCLUSIVE_PROGRAM_RETIRED


def exclusive_cases() -> list[Case]:
    cases: list[Case] = []
    index = 0
    # Every single-register load and store form, paired so that each load
    # form (order x result kept or dropped) and each store form (order x
    # source zero x status dropped) is the program of some case.
    for size in range(4):
        loads = [(acquire, register) for acquire in (0, 1)
                 for register in (4, 31)]
        stores = [(release, source, status) for release in (0, 1)
                  for source in (3, 31) for status in (5, 31)]
        for position, (release, source, status) in enumerate(stores):
            acquire, register = loads[position % len(loads)]
            index += 1
            byte = size_bytes(size)
            address = ACCESS + 0x100 + byte * (index % 4)
            load = exclusive_load_word(size, acquire, 2, register)
            store = exclusive_store_word(size, release, status, 2, source)

            def effect(memory, registers, size=size, address=address,
                       register=register, source=source, status=status):
                offset = address - REGION
                byte = size_bytes(size)
                old = int.from_bytes(memory[offset:offset + byte], "little")
                new = 0 if source == 31 else registers[source] & mask(size)
                memory[offset:offset + byte] = new.to_bytes(byte, "little")
                if register != 31:
                    registers[register] = old
                if status != 31:
                    registers[status] = 0

            name = (f"{'ldaxr' if acquire else 'ldxr'}{SIZE_NAMES_B[size]} "
                    f"{text(register, size)}, [x2]; "
                    f"{'stlxr' if release else 'stxr'}{SIZE_NAMES_B[size]} "
                    f"{register_text(status, False)}, "
                    f"{text(source, size)}, [x2]")
            cases.append(Case(
                f"{name} tagged 0x{TAGS[index % len(TAGS)]:02x}",
                exclusive_program(load, store, status),
                [], {2: tagged(address, index), 3: X_SOURCE}, effect,
                retired=exclusive_retired(status)))
    # The pair forms, the same way. LDXP into one dropped register is a form
    # of its own; into two is UNDEFINED.
    for wide in (0, 1):
        loads = [(acquire, first, second) for acquire in (0, 1)
                 for first, second in ((4, 6), (31, 6), (4, 31))]
        stores = [(release, first, second, status) for release in (0, 1)
                  for first in (3, 31) for second in (7, 31)
                  for status in (5, 31)]
        for position, (release, first, second, status) in enumerate(stores):
            acquire, load_first, load_second = loads[position % len(loads)]
            index += 1
            element = 4 << wide
            address = ACCESS + 0x140 + 2 * element * (index % 3)
            load = exclusive_load_pair_word(wide, acquire, 2, load_first,
                                            load_second)
            store = exclusive_store_pair_word(wide, release, status, 2, first,
                                              second)

            def effect(memory, registers, address=address, element=element,
                       load_first=load_first, load_second=load_second,
                       first=first, second=second, status=status):
                offset = address - REGION
                limit = (1 << 8 * element) - 1
                old = [int.from_bytes(memory[offset + element * lane:
                                             offset + element * (lane + 1)],
                                      "little") for lane in range(2)]
                new = [0 if first == 31 else registers[first] & limit,
                       0 if second == 31 else registers[second] & limit]
                for lane in range(2):
                    memory[offset + element * lane:
                           offset + element * (lane + 1)] = \
                        new[lane].to_bytes(element, "little")
                if load_first != 31:
                    registers[load_first] = old[0]
                if load_second != 31:
                    registers[load_second] = old[1]
                if status != 31:
                    registers[status] = 0

            def pair(a: int, b: int) -> str:
                return (f"{register_text(a, bool(wide))}, "
                        f"{register_text(b, bool(wide))}")
            name = (f"{'ldaxp' if acquire else 'ldxp'} "
                    f"{pair(load_first, load_second)}, [x2]; "
                    f"{'stlxp' if release else 'stxp'} "
                    f"{register_text(status, False)}, "
                    f"{pair(first, second)}, [x2]")
            cases.append(Case(
                f"{name} tagged 0x{TAGS[index % len(TAGS)]:02x}",
                exclusive_program(load, store, status),
                [], {2: tagged(address, index), 3: X_SOURCE, 7: X_OTHER},
                effect, retired=exclusive_retired(status)))

    # The monitor guards a location: a store-exclusive through a pointer with
    # another tag, or none, completes the pair the load opened.
    def monitor(name: str, size: int, load_tag: int | None,
                store_tag: int | None) -> None:
        nonlocal index
        index += 1
        byte = size_bytes(size)
        address = ACCESS + 0x180 + byte * (index % 4)
        load = exclusive_load_word(size, 0, 2, 4)
        store = exclusive_store_word(size, 0, 5, 8, 3)

        def pointer(tag: int | None) -> int:
            return address if tag is None else (tag << 56) | address

        def effect(memory, registers, size=size, address=address):
            offset = address - REGION
            byte = size_bytes(size)
            registers[4] = int.from_bytes(memory[offset:offset + byte],
                                          "little")
            memory[offset:offset + byte] = \
                (X_SOURCE & mask(size)).to_bytes(byte, "little")
            registers[5] = 0

        cases.append(Case(
            f"{name} {SIZE_NAMES_B[size] or SIZE_NAMES[size]} "
            f"[x2] and [x8]",
            exclusive_program(load, store, 5), [],
            {2: pointer(load_tag), 8: pointer(store_tag), 3: X_SOURCE},
            effect, retired=EXCLUSIVE_PROGRAM_RETIRED))

    for size in (0, 1, 2, 3):
        monitor("ldxr tag 0xb4, stxr tag 0x2c", size, 0xB4, 0x2C)
        monitor("ldxr tag 0x5a, stxr untagged", size, 0x5A, None)
        monitor("ldxr untagged, stxr tag 0xff", size, None, 0xFF)

    def pair_monitor(name: str, wide: int, load_tag: int | None,
                     store_tag: int | None) -> None:
        nonlocal index
        index += 1
        element = 4 << wide
        address = ACCESS + 0x1C0 + 2 * element * (index % 2)
        load = exclusive_load_pair_word(wide, 0, 2, 4, 6)
        store = exclusive_store_pair_word(wide, 0, 5, 8, 3, 7)

        def pointer(tag: int | None) -> int:
            return address if tag is None else (tag << 56) | address

        def effect(memory, registers, address=address, element=element):
            offset = address - REGION
            limit = (1 << 8 * element) - 1
            old = [int.from_bytes(memory[offset + element * lane:
                                         offset + element * (lane + 1)],
                                  "little") for lane in range(2)]
            for lane, value in enumerate((registers[3], registers[7])):
                memory[offset + element * lane:
                       offset + element * (lane + 1)] = \
                    (value & limit).to_bytes(element, "little")
            registers[4], registers[6] = old
            registers[5] = 0

        cases.append(Case(
            f"{name} {'x' if wide else 'w'} [x2] and [x8]",
            exclusive_program(load, store, 5), [],
            {2: pointer(load_tag), 8: pointer(store_tag), 3: X_SOURCE,
             7: X_OTHER}, effect, retired=EXCLUSIVE_PROGRAM_RETIRED))

    for wide in (0, 1):
        pair_monitor("ldxp tag 0xb4, stxp tag 0x2c", wide, 0xB4, 0x2C)
        pair_monitor("ldxp tag 0x5a, stxp untagged", wide, 0x5A, None)
        pair_monitor("ldxp untagged, stxp tag 0xff", wide, None, 0xFF)

    # A store-exclusive with no load before it fails, whatever its pointer's
    # tag, and writes nothing.
    for size in (0, 3):
        index += 1
        byte = size_bytes(size)
        address = ACCESS + 0x1E0
        store = exclusive_store_word(size, 0, 5, 2, 3)

        def effect(memory, registers):
            registers[5] = 1

        cases.append(Case(
            f"clrex; stxr{SIZE_NAMES_B[size]} w5, {text(3, size)}, [x2] "
            f"tagged 0x{TAGS[index % len(TAGS)]:02x} without a load",
            [CLREX, store, SVC], [], {2: tagged(address, index),
                                      3: X_SOURCE}, effect))

    # Alignment faults, with the tag on the base: a load faults as a read, a
    # store as a write, at the untagged address.
    for size in (1, 2, 3):
        index += 1
        address = ACCESS + CROSSING[size_bytes(size)]
        cases.append(Case(
            f"ldxr{SIZE_NAMES_B[size]} {text(4, size)}, [x2] tagged "
            f"0x{TAGS[index % len(TAGS)]:02x} across a block",
            [exclusive_load_word(size, 0, 2, 4), SVC], [],
            {2: tagged(address, index)}, fault=(address, "read")))
        index += 1
        cases.append(Case(
            f"stlxr{SIZE_NAMES_B[size]} w5, {text(3, size)}, [x2] tagged "
            f"0x{TAGS[index % len(TAGS)]:02x} across a block",
            [exclusive_store_word(size, 1, 5, 2, 3), SVC], [],
            {2: tagged(address, index), 3: X_SOURCE},
            fault=(address, "write")))
    for wide in (0, 1):
        index += 1
        total = 8 << wide
        address = ACCESS + (CROSSING[total] if wide == 0 else 8)
        cases.append(Case(
            f"ldaxp {register_text(4, bool(wide))}, "
            f"{register_text(6, bool(wide))}, [x2] tagged "
            f"0x{TAGS[index % len(TAGS)]:02x} across a block",
            [exclusive_load_pair_word(wide, 1, 2, 4, 6), SVC], [],
            {2: tagged(address, index)}, fault=(address, "read")))
        index += 1
        cases.append(Case(
            f"stxp w5, {register_text(3, bool(wide))}, "
            f"{register_text(7, bool(wide))}, [x2] tagged "
            f"0x{TAGS[index % len(TAGS)]:02x} across a block",
            [exclusive_store_pair_word(wide, 0, 5, 2, 3, 7), SVC], [],
            {2: tagged(address, index), 3: X_SOURCE, 7: X_OTHER},
            fault=(address, "write")))
    return cases


# --------------------------------------------------------------------------
# Section: translation faults and read-only memory, and TBI off


def fault_cases() -> list[Case]:
    """A tagged access to an unmapped or read-only page faults at the
    untagged address; the same pointer with TBI off faults at the pointer."""
    cases: list[Case] = []
    index = 0
    reads = [
        ("ldar x4, [x2]", ordered_word(3, True, 2, 4), []),
        ("ldarb w4, [x2]", ordered_word(0, True, 2, 4), []),
        ("ldapr x4, [x2]", ldapr_word(3, 2, 4), ["lrcpc"]),
        ("ldapur w4, [x2, #4]", ldapur_word(2, 1, 4, 2, 4), ["lrcpc2"]),
        ("ldxr x4, [x2]", exclusive_load_word(3, 0, 2, 4), []),
        ("ldaxrh w4, [x2]", exclusive_load_word(1, 1, 2, 4), []),
        ("ldxp x4, x6, [x2]", exclusive_load_pair_word(1, 0, 2, 4, 6), []),
    ]
    writes = [
        ("ldadd x3, x4, [x2]", rmw_word(3, 0, 0, 3, 0, 2, 4), ["lse"]),
        ("ldsmaxalb w3, w4, [x2]", rmw_word(0, 1, 1, 3, 4, 2, 4), ["lse"]),
        ("stclr x3, [x2]", rmw_word(3, 0, 0, 3, 1, 2, 31), ["lse"]),
        ("swpl w3, w4, [x2]", rmw_word(2, 0, 1, 3, 8, 2, 4), ["lse"]),
        ("casal x3, x4, [x2]", cas_word(3, 1, 1, 3, 2, 4), ["lse"]),
        ("caspal x4, x5, x6, x7, [x2]", casp_word(1, 1, 1, 4, 2, 6), ["lse"]),
        ("stlr x3, [x2]", ordered_word(3, False, 2, 3), []),
        ("stlrh w3, [x2]", ordered_word(1, False, 2, 3), []),
        ("stlur x3, [x2, #8]", ldapur_word(3, 0, 8, 2, 3), ["lrcpc2"]),
    ]
    for table, access in ((reads, "read"), (writes, "write")):
        for name, word, native in table:
            for label, address, read_only in (
                    ("unmapped", UNMAPPED, False),
                    ("read-only", READ_ONLY_REGION + 0x40, True)):
                if access == "read" and read_only:
                    continue
                index += 1
                immediate = re.search(r"#(-?\d+)\]", name)
                offset = int(immediate.group(1)) if immediate else 0
                registers = {2: tagged(address - offset, index),
                             3: X_SOURCE, 7: X_OTHER}
                cases.append(Case(
                    f"{name} tagged 0x{TAGS[index % len(TAGS)]:02x} to a "
                    f"{label} page", [word, SVC], native, registers,
                    fault=(address, access), read_only=read_only))
    # The same pointers with TBI off: the tag is part of the address, so the
    # access faults at the pointer, aligned or not.
    controls = [
        ("ldadd x3, x4, [x2]", rmw_word(3, 0, 0, 3, 0, 2, 4), ["lse"],
         "write", 8, 12),
        ("swpab w3, w4, [x2]", rmw_word(0, 1, 0, 3, 8, 2, 4), ["lse"],
         "write", 1, 1),
        ("casl w3, w4, [x2]", cas_word(2, 0, 1, 3, 2, 4), ["lse"], "write",
         4, 13),
        ("casp x4, x5, x6, x7, [x2]", casp_word(1, 0, 0, 4, 2, 6), ["lse"],
         "write", 16, 8),
        ("ldar x4, [x2]", ordered_word(3, True, 2, 4), [], "read", 8, 12),
        ("stlrh w3, [x2]", ordered_word(1, False, 2, 3), [], "write", 2, 15),
        ("ldapr w4, [x2]", ldapr_word(2, 2, 4), ["lrcpc"], "read", 4, 14),
        ("stlur x3, [x2]", ldapur_word(3, 0, 0, 2, 3), ["lrcpc2"], "write",
         8, 12),
        ("ldxr x4, [x2]", exclusive_load_word(3, 0, 2, 4), [], "read", 8, 12),
        ("stxr w5, x3, [x2]", exclusive_store_word(3, 0, 5, 2, 3), [],
         "write", 8, 12),
        ("ldaxp w4, w6, [x2]", exclusive_load_pair_word(0, 1, 2, 4, 6), [],
         "read", 8, 12),
        ("stlxp w5, x3, x7, [x2]", exclusive_store_pair_word(1, 1, 5, 2, 3, 7),
         [], "write", 16, 8),
    ]
    for name, word, native, access, size, crossing in controls:
        for label, offset in (("aligned", 0x20), ("across a block", crossing)):
            if label == "across a block" and size == 1:
                continue
            index += 1
            address = ACCESS + (offset if label != "aligned" else 0x20)
            pointer = tagged(address, index)
            # An aligned access translates and faults at the pointer; one
            # across a block is misaligned before it translates, and faults
            # at the pointer as well.
            cases.append(Case(
                f"{name} tagged 0x{TAGS[index % len(TAGS)]:02x} with "
                f"top-byte ignore off, {label}", [word, SVC], native,
                {2: pointer, 3: X_SOURCE, 7: X_OTHER},
                fault=(pointer, access), tbi=False))
    return cases


# Sections, in the order their cases are laid out. A section is a function
# that returns its cases; adding a section adds a name here.
SECTIONS = (rmw_cases, cas_cases, casp_cases, ordered_cases, rcpc_cases,
            exclusive_cases, fault_cases)


# --------------------------------------------------------------------------
# Record assembly


def program_text(words: list[int]) -> list:
    return [[4 * offset, f"{word:08x}"] for offset, word in enumerate(words)]


def case_item(case: Case, program: int, blob_digest: str) -> dict:
    initial: dict[str, str] = {}
    registers = list(corpus.FILL_REGISTERS)
    for number, value in case.registers.items():
        registers[number] = value & MASK64
    for number in range(31):
        if registers[number] != corpus.FILL_REGISTERS[number]:
            initial[f"x{number}"] = corpus.hex64(registers[number])
    if case.tbi:
        initial["tbi_enabled"] = corpus.hex64(1)
    expected: dict[str, str] = {}
    memory = bytearray(page_blob())
    if case.fault is None:
        after = dict(enumerate(registers))
        case.effect(memory, after)
        for number in range(31):
            value = after[number] & MASK64
            if value != registers[number]:
                expected[f"x{number}"] = corpus.hex64(value)
        expected["pc"] = corpus.hex64(corpus.CODE_BASE + 4 * len(case.words))
    flags = READ_ONLY_FLAGS if case.read_only else READ_WRITE_FLAGS
    address = READ_ONLY_REGION if case.read_only else REGION
    item = {
        "name": case.name, "program": program,
        "event": "memory-fault" if case.fault else "system-call",
        "ordinals": [0, 0], "invalid_encoding": False,
        "minimization": "full", "native": case.native,
        "initial": initial, "expected": expected,
        "retired": case.retired,
        "memory": [{
            "address": corpus.hex64(address), "size": PAGE,
            "flags": flags, "blob": blob_digest,
            "expected": [[offset, data.hex()] for offset, data in
                         corpus.runs_between(page_blob(), bytes(memory))],
            "tags": [], "expected_tags": []}],
    }
    if case.fault:
        item["fault"] = {"address": corpus.hex64(case.fault[0]),
                         "access": case.fault[1]}
    return item


@functools.cache
def all_cases() -> tuple[Case, ...]:
    cases = [case for section in SECTIONS for case in section()]
    names = [case.name for case in cases]
    if len(set(names)) != len(names):
        duplicate = next(name for name in names if names.count(name) > 1)
        raise SystemExit(f"two cases are named {duplicate!r}")
    return tuple(cases)


@functools.cache
def build() -> tuple[dict, str]:
    """(the record, the digest of the page blob its regions name)."""
    digest = hashlib.sha256(page_blob()).hexdigest()
    programs: list[tuple[int, ...]] = []
    items = []
    for case in all_cases():
        words = tuple(case.words)
        if words not in programs:
            programs.append(words)
        items.append(case_item(case, programs.index(words), digest))
    record = {
        "record": f"{PRODUCER}/4k", "producer": PRODUCER, "site": SITE,
        "profiles": ["4k"], "options": [],
        "programs": [{"entry": 0, "words": program_text(list(words))}
                     for words in programs],
        "cases": items,
    }
    return record, digest


def counts() -> dict[str, int]:
    """The cases by kind: completing, faulting with TBI on, and the TBI-off
    controls."""
    cases = all_cases()
    return {
        "cases": len(cases),
        "fault_cases": sum(1 for case in cases if case.fault and case.tbi),
        "tbi_off_cases": sum(1 for case in cases if not case.tbi),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dump", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    options = parser.parse_args()
    lines = options.dump.read_text(encoding="utf-8").splitlines()
    header = json.loads(lines[0])
    kept = [line for line in lines[1:] if line.strip() and
            not (line.startswith('{"cases"') and
                 json.loads(line)["producer"] == PRODUCER)]
    record, digest = build()
    known = {match.group(1) for line in kept
             if (match := re.match(r'\{"blob": "([0-9a-f]{64})"', line))}
    fresh = []
    if digest not in known:
        fresh.append(json.dumps({"blob": digest,
                                 "zlib": zlib.compress(page_blob(), 9).hex()},
                                sort_keys=True))
    summary = counts()
    header["meta"]["tbi_memory"] = {
        "script": SITE,
        "sha256": hashlib.sha256(pathlib.Path(__file__).read_bytes())
        .hexdigest(), **summary,
    }
    with options.output.open("w", encoding="utf-8") as sink:
        sink.write(json.dumps(header, sort_keys=True) + "\n")
        for line in kept:
            sink.write(line + "\n")
        for line in fresh:
            sink.write(line + "\n")
        sink.write(json.dumps(record, sort_keys=True) + "\n")
    print(f"appended {summary['cases']} top-byte-ignore memory cases "
          f"({summary['fault_cases']} faulting, {summary['tbi_off_cases']} "
          f"with TBI off)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
