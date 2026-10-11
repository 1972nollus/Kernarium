#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Author the SM = 0 vector-length cases of the A64 instruction corpus.

usage: sm0-vl-corpus.py DUMP OUTPUT

A virtual CPU with FEAT_SME and without FEAT_SVE executes CNTB, CNTH, CNTW and
CNTD, RDVL, ADDVL, ADDPL, and the scalar INCB, INCH, INCW, INCD, DECB, DECH,
DECW and DECD outside streaming mode, every predicate pattern and multiplier,
for one constant vector length of 128 bits (CNTB = 16, CNTD = 2). The
architecture makes every one of them UNDEFINED at PSTATE.SM = 0 on such a CPU,
and an M4 traps them, so no hardware result exists: these cases are
PROJECT-DEFINED, and the expectation is the instructions' pseudocode with the
constant length, written here independently of the engines. See
darwin/cpu/arm64/detail/sm0_vector_length.hpp and docs/architecture.md.

The cases go into one arm64-sm0-vl record under without_sve, which the runner
translates for on every host, and the record's SVL makes the runner run them
only where the virtual CPU implements the SME profile (the A64 instructions
the engines translate them to run on any hardware, so a host without SME runs
them with the profile assumed; see arm64-instruction-corpus-sm0-vl). The
streaming-mode meaning of the same words, with the streaming vector length,
belongs to the native SME records, and sme-native-corpus.py leaves out the
cases whose results the SME host's trap would contradict. The same words are
UNDEFINED where neither SME nor SVE is implemented, which arm64-absent-sve
under without_sme and without_sve pins, and every other SVE word at SM = 0
stays UNDEFINED under the arm64-absent-sve records.

Run it on a finished dump, after the step that appended the SVE trap records,
and --repack the result; the invalid-encoding step still runs last.
"""
from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import functools
import hashlib
import importlib.util
import json
import pathlib
import sys

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
sme_table = _load("sme_encoding_table", "sme_encoding_table.py")

SITE = "tools/arm64-tests/sm0-vl-corpus.py"
PRODUCER = "arm64-sm0-vl"
WITHOUT_SVE = "without_sve"
# The streaming vector length the record's SME state has, as the native SME
# records do; it is the profile the virtual CPU implements, not what the
# family counts.
SVL = 64
SVC = 0xD400_0001
ZA = corpus.SVCR_ZA
# The constant non-streaming vector length, in bytes.
VL = 16


def pattern_count(pattern: int, elements: int) -> int:
    """DecodePredCount()."""
    if pattern == 0:  # POW2
        count = 1 << (elements.bit_length() - 1)
    elif 1 <= pattern <= 8:  # VL1 to VL8
        count = pattern
    elif 9 <= pattern <= 13:  # VL16 to VL256
        count = 16 << (pattern - 9)
    elif pattern == 29:  # MUL4
        count = elements - elements % 4
    elif pattern == 30:  # MUL3
        count = elements - elements % 3
    elif pattern == 31:  # ALL
        count = elements
    else:
        count = 0
    return count if count <= elements else 0


def signed(value: int, bits: int) -> int:
    return value - (1 << bits) if value >> (bits - 1) else value


def cnt(size: int, pattern: int, multiplier: int, rd: int) -> int:
    return (0x0420_E000 | size << 22 | (multiplier - 1) << 16 |
            pattern << 5 | rd)


def inc_dec(decrement: bool, size: int, pattern: int, multiplier: int,
            rdn: int) -> int:
    return (0x0430_E000 | (0x400 if decrement else 0) | size << 22 |
            (multiplier - 1) << 16 | pattern << 5 | rdn)


def rdvl(immediate: int, rd: int) -> int:
    return 0x04BF_5000 | (immediate & 63) << 5 | rd


def add_length(predicate: bool, rd: int, rn: int, immediate: int) -> int:
    return ((0x0460_5000 if predicate else 0x0420_5000) | rn << 16 |
            (immediate & 63) << 5 | rd)


class Registers:
    """The general registers of the base fill; 31 is SP."""

    def __init__(self) -> None:
        self.value = list(corpus.FILL_REGISTERS)

    def get(self, number: int, sp: bool) -> int:
        return self.value[31] if number == 31 and sp else \
            0 if number == 31 else self.value[number]

    def set(self, number: int, value: int, sp: bool) -> None:
        if number != 31 or sp:
            self.value[number] = value & (1 << 64) - 1


def execute(word: int) -> Registers:
    """The registers after a family word, from the instructions' pseudocode
    with a vector length of VL bytes."""
    registers = Registers()
    rd = word & 31
    rn = word >> 16 & 31
    size = word >> 22 & 3
    multiplier = (word >> 16 & 15) + 1
    pattern = word >> 5 & 31
    immediate = signed(word >> 5 & 63, 6)
    count = pattern_count(pattern, VL // (1 << size)) * multiplier
    if word & 0xFF30_FC00 == 0x0420_E000:  # CNT[BHWD]
        registers.set(rd, count, False)
    elif word & 0xFF30_FC00 in (0x0430_E000, 0x0430_E400):  # INC, DEC
        delta = -count if word & 0x400 else count
        registers.set(rd, registers.get(rd, False) + delta, False)
    elif word & 0xFFFF_F800 == 0x04BF_5000:  # RDVL
        registers.set(rd, immediate * VL, False)
    elif word & 0xFFA0_F800 == 0x0420_5000:  # ADDVL, ADDPL
        scale = VL // 8 if word & 0x0040_0000 else VL
        registers.set(rd, registers.get(rn, True) + immediate * scale, True)
    else:
        raise SystemExit(f"{word:08x}: not a vector-length word")
    return registers


def register_name(number: int, sp: bool) -> str:
    return ("SP" if sp else "XZR") if number == 31 else f"X{number}"


def operands(word: int) -> str:
    rd = word & 31
    rn = word >> 16 & 31
    pattern = word >> 5 & 31
    multiplier = (word >> 16 & 15) + 1
    immediate = signed(word >> 5 & 63, 6)
    if word & 0xFFFF_F800 == 0x04BF_5000:
        return f"{register_name(rd, False)}, #{immediate}"
    if word & 0xFFA0_F800 == 0x0420_5000:
        return (f"{register_name(rd, True)}, {register_name(rn, True)}, "
                f"#{immediate}")
    return (f"{register_name(rd, False)}, pattern {pattern}, "
            f"MUL #{multiplier}")


def words() -> list[tuple[int, int]]:
    """(word, SVCR) of every case: each element size, every predicate
    pattern, including the reserved ones that count nothing, and three
    multipliers of CNT, INC and DEC, over destinations that rotate through
    the registers; RDVL and ADDVL and ADDPL for every immediate; the
    destination XZR where it discards the result and SP where it names the
    stack; and one word of each form with ZA enabled."""
    items: list[tuple[int, int]] = []
    rotation = 0

    def register() -> int:
        nonlocal rotation
        rotation += 1
        number = rotation * 7 % 30
        return number + 1 if number == 18 else number  # not X18
    for size in range(4):
        for pattern in range(32):
            for multiplier in (1, 3, 16):
                items.append((cnt(size, pattern, multiplier, register()), 0))
                items.append((inc_dec(False, size, pattern, multiplier,
                                      register()), 0))
                items.append((inc_dec(True, size, pattern, multiplier,
                                      register()), 0))
        items.append((cnt(size, 31, 1, 31), 0))
        items.append((inc_dec(False, size, 31, 1, 31), 0))
        items.append((inc_dec(True, size, 31, 1, 31), 0))
    for immediate in range(-32, 32):
        items.append((rdvl(immediate, register()), 0))
        for predicate in (False, True):
            for rd, rn in ((1, 2), (3, 3), (31, 4), (5, 31), (31, 31)):
                items.append((add_length(predicate, rd, rn, immediate), 0))
    items.append((rdvl(1, 31), 0))
    for word in (cnt(3, 31, 1, 0), cnt(0, 31, 1, 30), inc_dec(False, 1, 31, 2, 7),
                 inc_dec(True, 2, 5, 3, 8), rdvl(-5, 9), add_length(False, 31, 31, 3),
                 add_length(True, 31, 6, -4)):
        items.append((word, ZA))
    return items


@functools.cache
def table():
    return sme_table.load()


def case_for(word: int, svcr: int, program: int) -> dict:
    registers = execute(word)
    expected = {"pc": corpus.hex64(corpus.CODE_BASE + 8)}
    for number in range(32):
        if registers.value[number] != corpus.FILL_REGISTERS[number]:
            expected["sp" if number == 31 else f"x{number}"] = \
                corpus.hex64(registers.value[number])
    row = table().row_for(word)
    if row is None or not sme_table.sm0_vector_length_word(word):
        raise SystemExit(f"{word:08x}: not a row of the family")
    name = (f"{row.stable_id} {row.encoding} {word:08x} {operands(word)} "
            f"at SM = 0{', ZA = 1' if svcr & ZA else ''} (project-defined)")
    return {
        "name": name, "program": program, "event": "system-call",
        "ordinals": [0, 0], "invalid_encoding": False,
        "minimization": "full", "native": [],
        "initial": {"svcr": corpus.hex64(svcr)} if svcr else {},
        "expected": expected, "retired": 2,
    }


def record() -> dict:
    programs = []
    cases = []
    for word, svcr in words():
        programs.append({"entry": 0, "words": [[0, f"{word:08x}"],
                                               [4, f"{SVC:08x}"]]})
        cases.append(case_for(word, svcr, len(programs) - 1))
    bits = 1 << corpus.OPTION_NAMES.index(WITHOUT_SVE)
    return {
        "record": f"{PRODUCER}/options={bits:x}", "producer": PRODUCER,
        "site": SITE, "profiles": ["16k", "4k"], "options": [WITHOUT_SVE],
        "svl": SVL, "programs": programs, "cases": cases,
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
    item = record()
    header["meta"]["sm0_vl"] = {
        "script": SITE,
        "sha256": hashlib.sha256(pathlib.Path(__file__).read_bytes())
        .hexdigest(),
        "table": {"file": sme_table.TABLE_PATH, "sha256": table().sha256},
        "svl": SVL, "vector_length": VL, "cases": len(item["cases"]),
    }
    with options.output.open("w", encoding="utf-8") as sink:
        sink.write(json.dumps(header, sort_keys=True) + "\n")
        for line in kept:
            sink.write(line + "\n")
        sink.write(json.dumps(item, sort_keys=True) + "\n")
    print(f"appended {len(item['cases'])} project-defined SM = 0 "
          "vector-length cases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
