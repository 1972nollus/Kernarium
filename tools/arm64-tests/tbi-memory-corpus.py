#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Author the top-byte-ignore memory cases of the A64 instruction corpus.

usage: tbi-memory-corpus.py DUMP OUTPUT

With top-byte ignore (TBI) the hardware ignores the top byte of a data
address: a pointer with a tag in bits 63:56 reaches the byte its low 56 bits
name, a fault reports that address, and the base register keeps its tag.
Android runs every process this way (Scudo and bionic's heap tagging), and the
virtual CPU does whenever the host passes --guest-top-byte-ignore
(CpuState::tbi_enabled). Without TBI a tagged pointer is simply an address
outside the guest's range and faults at that address.

The cases go into the arm64-tbi-memory and arm64-tbi-equivalence records.
Each section below owns one family of memory forms and its functions are named
for it; a section never reaches into another's:

- equivalence: the loads and stores of general and SIMD&FP registers (every
  addressing mode, pairs, structures, DC ZVA) and of the SVE and SME memory
  forms, each an existing corpus case that reaches memory through an untagged
  base register, rerun with the base tagged. A tagged pointer reaches the same
  bytes, so the expected state, memory, retirement and fault address are the
  source case's, whose goldens come from the architecture's pseudocode or the
  hardware. The script derives only the base register's tag, which a writeback
  keeps, and the tag byte, a fixed rotation. The fault address of a case that
  does not complete stays the untagged one;
- the register-offset forms with the tag in the index register, and with the
  sum of base and index carrying into the tag byte;
- SP as the base: the sources that name SP, tagged like any other base, and
  one source rewritten to name SP, where a tagged SP completes as the source
  did, since TBI ignores the top byte of every data address, SP-based or not;
  and
- the controls with TBI off, where a tagged pointer, SP or not, faults at the
  pointer.

Every case is derived from the dump, so a source case that a regeneration adds
or drops changes the records without a change to this script. A source is a
single memory instruction, or a straight run of them off one base, whose base
register is not also one of its operands. The exclusive, atomic,
compare-and-swap, ordered and RCpc forms have sections of their own, and MOPS,
which updates its registers in place, is not a source.

Run it on a finished dump, after the SM = 0 family step, and --repack the
result; the 16 KiB retirement and invalid-encoding steps still run last.
"""
from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import collections
import copy
import csv
import dataclasses
import hashlib
import importlib.util
import json
import pathlib
import re
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

SITE = "tools/arm64-tests/tbi-memory-corpus.py"
SVC = 0xD400_0001
MASK64 = (1 << 64) - 1
LOW56 = (1 << 56) - 1

# Tag bytes cycle through these. Bit 55 selects the translation table on
# hardware, so no tag sets a bit below bit 56.
TAGS = (0xB4, 0x5A, 0x01, 0xFF, 0x80, 0x2C, 0x7F, 0xA5)


# --------------------------------------------------------------------------
# Section: equivalence of tagged and untagged bases

EQUIVALENCE = "arm64-tbi-equivalence"
# Each class of sources, by instruction shape, base-register range, event and
# record options, contributes its first and last source and an even spread
# between, up to this many.
PER_CLASS = 3
# Each instruction form contributes this many cases of each variant.
PER_VARIANT = 1
VARIANTS = ("control", "sp", "sp control", "index", "carry")


@dataclasses.dataclass(frozen=True)
class Form:
    """A memory instruction word: its kind, where its base register is, which
    general registers it also reads or writes, and whether it stores."""

    kind: str
    base: int
    # General registers that, when equal to the base, would make the tag part
    # of the instruction's result: a data register, an index or an increment.
    operands: tuple[int, ...]
    stores: bool
    label: str = ""


def scale_of(word: int) -> int:
    """The log2 of the bytes one single-register transfer moves."""
    size = word >> 30
    if word >> 26 & 1 and word >> 23 & 1 and size == 0:
        return 4
    return size


def single_form(word: int) -> Form | None:
    """LDR, STR, the sign-extending loads, LDUR, STUR, LDTR, STTR and the
    pre- and post-indexed and register-offset forms, of a general or SIMD&FP
    register; not the prefetches, which move nothing."""
    selector = word >> 24 & 3
    vector = word >> 26 & 1
    size = word >> 30
    opc = word >> 22 & 3
    if not vector and size == 3 and opc == 2:
        return None
    operands: tuple[int, ...] = () if vector else (word & 31,)
    if selector == 1:
        kind = "unsigned"
    elif selector == 0 and not word >> 21 & 1:
        kind = "immediate"
    elif selector == 0 and word >> 10 & 3 == 2:
        kind = "register"
        operands += (word >> 16 & 31,)
    else:
        return None
    stores = (opc & 1) == 0 if vector else opc == 0
    return Form(kind, word >> 5 & 31, operands, stores)


def pair_form(word: int) -> Form | None:
    opc = word >> 30
    vector = word >> 26 & 1
    load = word >> 22 & 1
    if opc == 3 or (opc == 1 and not vector and not load):
        return None
    operands: tuple[int, ...] = () if vector else (word & 31, word >> 10 & 31)
    return Form("pair", word >> 5 & 31, operands, not load)


def structure_form(word: int) -> Form | None:
    operands: tuple[int, ...] = ()
    if word >> 23 & 1 and word >> 16 & 31 != 31:
        operands = (word >> 16 & 31,)
    return Form("structure", word >> 5 & 31, operands, not word >> 22 & 1)


def zva_form(word: int) -> Form | None:
    """DC ZVA, whose address register is the transfer register."""
    if word & 0xFFFF_FFE0 != 0xD50B_7420:
        return None
    return Form("zva", word & 31, (), True)


def base_form(word: int) -> Form | None:
    """The form of a load or store of general or SIMD&FP registers, or None."""
    if (form := zva_form(word)) is not None:
        return form
    if word >> 24 & 0xBF in (0x0C, 0x0D):
        return structure_form(word)
    group = word >> 27 & 7
    if group == 5 and not word >> 25 & 1:
        return pair_form(word)
    if group == 7:
        return single_form(word)
    return None


def mode_word(word: int) -> bool:
    """SMSTART and SMSTOP, with which a program may enter its mode."""
    return word & 0xFFFF_F0FF == 0xD503_407F


class Vectors:
    """The SVE and SME memory entries of the instruction inventory, which
    credit a word to its form."""

    def __init__(self) -> None:
        self.classifier = corpus.Classifier()
        self.labels: dict[str, str] = {}
        with corpus.INVENTORY.open(encoding="utf-8") as source:
            for row in csv.DictReader(source, delimiter="\t"):
                if row["current_suite"] in ("shared-sve-memory",
                                            "shared-sme-memory"):
                    self.labels[row["stable_id"]] = row["pdf_label"]

    def form(self, word: int) -> Form | None:
        # SVE and SME memory words have bit 31 set and 0 or 2 in bits 28:25.
        if not word >> 31 or word >> 25 & 0xF not in (0, 2):
            return None
        labels = [self.labels[stable_id]
                  for stable_id in self.classifier.classify(word)
                  if stable_id in self.labels]
        if len(labels) != 1:
            return None
        label = labels[0]
        if label.startswith(("PRF", "LDFF1", "LDNF1")) or \
                "plus vector" in label or "vector plus" in label:
            return None
        operands = (word >> 16 & 31,) if "scalar plus scalar" in label else ()
        return Form("vector", word >> 5 & 31, operands,
                    label.startswith("ST"), label)


VECTORS: list[Vectors] = []


def form_of(word: int) -> Form | None:
    if (form := base_form(word)) is not None:
        return form
    if not VECTORS:
        VECTORS.append(Vectors())
    return VECTORS[0].form(word)


def sign_extended(value: int, bits: int) -> int:
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >> (bits - 1) else value


def register_value(initial: dict, number: int) -> int:
    if number == 31:
        return 0
    value = initial.get(f"x{number}")
    return int(value, 16) if value is not None else \
        corpus.FILL_REGISTERS[number]


def index_value(word: int, initial: dict) -> int:
    """The value a register-offset form adds to its base, before the shift,
    as an unsigned 64-bit number."""
    index = register_value(initial, word >> 16 & 31)
    option = word >> 13 & 7
    if option == 2:
        return index & 0xFFFF_FFFF
    if option == 6:
        return sign_extended(index, 32) & MASK64
    return index


def displacement(word: int, form: Form, initial: dict) -> int | None:
    """The distance from the base register to the first byte an instruction
    reaches, or None where this script does not derive it."""
    if form.kind in ("structure", "zva"):
        return 0
    if form.kind == "pair":
        if word >> 23 & 3 == 1:
            return 0
        opc = word >> 30
        scale = 2 + opc if word >> 26 & 1 else 2 + (opc >> 1)
        return sign_extended(word >> 15 & 0x7F, 7) << scale
    if form.kind == "unsigned":
        return (word >> 10 & 0xFFF) << scale_of(word)
    if form.kind == "immediate":
        return 0 if word >> 10 & 3 == 1 else sign_extended(word >> 12, 9)
    if form.kind == "register":
        shift = scale_of(word) if word >> 12 & 1 else 0
        return sign_extended(index_value(word, initial), 64) << shift
    # LDR and STR of Z and P registers at the zero offset.
    spill = word & 0xFFC0_E000 in (0x8580_4000, 0xE580_4000) or \
        word & 0xFFC0_E010 in (0x8580_0000, 0xE580_0000)
    offset = (word >> 16 & 63) << 3 | word >> 10 & 7
    return 0 if spill and offset == 0 else None


@dataclasses.dataclass
class Source:
    """A case the equivalence section reruns with a tagged base."""

    record: dict
    case: dict
    words: list[int]
    forms: list[Form]

    @property
    def bases(self) -> set[int]:
        return {form.base for form in self.forms}

    @property
    def single(self) -> bool:
        return len(self.forms) == 1


def program_words(record: dict, case: dict) -> list[int] | None:
    program = record["programs"][case["program"]]
    words = [int(word, 16) for _offset, word in program["words"]]
    if program["entry"] != 0 or [offset for offset, _word in
                                 program["words"]] != \
            [4 * index for index in range(len(words))]:
        return None
    return words


def as_source(record: dict, case: dict) -> Source | None:
    if case["event"] not in ("system-call", "memory-fault") or \
            case["invalid_encoding"] or \
            "tbi_enabled" in case["initial"] or "validator" in case:
        return None
    if any(region["tags"] or region["expected_tags"]
           for region in case.get("memory", [])):
        return None
    words = program_words(record, case)
    if words is None or len(words) < 2 or words[-1] != SVC:
        return None
    forms: list[Form] = []
    for word in words[:-1]:
        if mode_word(word) and not forms:
            continue
        form = form_of(word)
        if form is None:
            return None
        forms.append(form)
    if not forms:
        return None
    source = Source(record, case, words, forms)
    bases = source.bases
    # Operand 31 is the zero register, whatever the base.
    if any(bases & (set(form.operands) - {31}) for form in forms):
        return None
    for base in bases:
        value = case["initial"].get(key_of(base))
        if value is None or int(value, 16) >> 56:
            return None
    # A misaligned SP faults at SP, which the architecture does not say is
    # tagged.
    if 31 in bases and int(case["initial"]["sp"], 16) % 16:
        return None
    # Neither SP unless it is the base nor the thread pointer is rewritten,
    # so a case that changes one is not ours to derive.
    if ("sp" in case["expected"] and 31 not in bases) or \
            "tls" in case["expected"]:
        return None
    return source


def shape(word: int, form: Form) -> tuple:
    """The fields of an instruction word that choose its decoding: the width
    and kind of a transfer, the addressing mode, the structure count."""
    if form.kind == "unsigned":
        return (word >> 30, word >> 26 & 1, word >> 22 & 3)
    if form.kind == "immediate":
        return (word >> 30, word >> 26 & 1, word >> 22 & 3, word >> 10 & 3)
    if form.kind == "register":
        return (word >> 30, word >> 26 & 1, word >> 22 & 3,
                word >> 13 & 7, word >> 12 & 1)
    if form.kind == "pair":
        return (word >> 30, word >> 26 & 1, word >> 22 & 1, word >> 23 & 3)
    if form.kind == "structure":
        return (word >> 22 & 1, word >> 23 & 1, word >> 24 & 1,
                word >> 21 & 1, word >> 12 & 15, word >> 10 & 3)
    return (form.label,)


def class_key(source: Source) -> tuple:
    form = source.forms[0]
    return (form.kind, shape(source.words[0], form),
            tuple(sorted({base // 8 for base in source.bases})),
            source.case["event"], len(source.forms) > 1,
            source.case.get("fault", {}).get("access"),
            tuple(source.record["options"]), source.record.get("svl", 0))


def spread(count: int, wanted: int) -> list[int]:
    if count <= wanted:
        return list(range(count))
    return sorted({round(step * (count - 1) / (wanted - 1))
                   for step in range(wanted)})


def select(sources: list[Source]) -> list[Source]:
    classes: dict[tuple, list[Source]] = collections.OrderedDict()
    for source in sources:
        classes.setdefault(class_key(source), []).append(source)
    chosen = []
    for members in classes.values():
        chosen.extend(members[index]
                      for index in spread(len(members), PER_CLASS))
    return chosen


NAME = re.compile(r"^(?P<source>.*) \[tbi (?P<variant>[a-z ]+) "
                  r"0x(?P<tag>[0-9a-f]{2})\](?: #\d+)?$")


def derived_name(source: Source, variant: str, tag: int) -> str:
    return f"{source.case['name']} [tbi {variant} 0x{tag:02x}]"


def key_of(number: int) -> str:
    return "sp" if number == 31 else f"x{number}"


def tag_bases(source: Source, case: dict, tag: int) -> None:
    for number in source.bases:
        key = key_of(number)
        case["initial"][key] = corpus.hex64(
            int(case["initial"][key], 16) | tag << 56)
        # A writeback adds to the tagged register, so the tag adds to its
        # result, and a register increment's own top byte carries into it.
        if key in case["expected"]:
            case["expected"][key] = corpus.hex64(
                int(case["expected"][key], 16) + (tag << 56) & MASK64)


def fault_at(case: dict, address: int, form: Form) -> dict:
    """The case as a fault at an address: nothing retires and nothing
    changes."""
    case["event"] = "memory-fault"
    case["expected"] = {}
    case["retired"] = 0
    case["fault"] = {"address": corpus.hex64(address & MASK64),
                     "access": "write" if form.stores else "read"}
    for region in case.get("memory", []):
        region["expected"] = []
    return case


def derive(source: Source, variant: str, tag: int,
           other: int) -> tuple[list[int], dict] | None:
    """The program and case of one variant of a source, or None where the
    variant does not apply to it."""
    case = copy.deepcopy(source.case)
    words = list(source.words)
    initial = case["initial"]
    case["name"] = derived_name(source, variant, tag)
    case["ordinals"] = [0, 0]
    del case["program"]
    if variant == "base":
        tag_bases(source, case, tag)
        initial["tbi_enabled"] = corpus.hex64(1)
        return words, case
    if not source.single or case["event"] != "system-call" or \
            source.forms[0].kind == "zva":
        return None
    form = source.forms[0]
    word = words[0]
    base = key_of(form.base)
    value = int(initial[base], 16)
    reach = displacement(word, form, initial)
    if variant in ("control", "sp control") and reach is None:
        return None
    if variant == "control":
        # TBI is off: the tagged pointer is an address nothing maps.
        initial[base] = corpus.hex64(value | tag << 56)
        return words, fault_at(case, (value | tag << 56) + reach, form)
    if variant in ("sp", "sp control"):
        # The source with SP for its base register, which must be aligned.
        if form.base == 31 or value % 16:
            return None
        del initial[base]
        words[0] = word & ~(31 << 5) | 31 << 5
        initial["sp"] = corpus.hex64(value | tag << 56)
        if variant == "sp control":
            return words, fault_at(case, (value | tag << 56) + reach, form)
        if base in case["expected"]:
            case["expected"]["sp"] = corpus.hex64(
                int(case["expected"].pop(base), 16) + (tag << 56) & MASK64)
        initial["tbi_enabled"] = corpus.hex64(1)
        return words, case
    if form.kind != "register" or form.base == 31:
        return None
    number = word >> 16 & 31
    option = word >> 13 & 7
    shift = scale_of(word) if word >> 12 & 1 else 0
    # The index register is rewritten, so it is not the stored data either.
    if number == 31 or number == word & 31 and not word >> 26 & 1:
        return None
    index = register_value(initial, number)
    if variant == "index":
        if option not in (3, 7) or index >> 56:
            return None
        initial[f"x{number}"] = corpus.hex64(index | other << 56)
        initial["tbi_enabled"] = corpus.hex64(1)
        return words, case
    if variant == "carry":
        # The base sits a page below the top of the 56-bit range and the
        # index is what carries the sum round to the address the source
        # reached, so the sum's tag byte is not the base's.
        address = (value + (index_value(word, initial) << shift)) & MASK64
        if (address + 0x1000) % (1 << shift) or address + 0x1000 >= 1 << 31:
            return None
        initial[base] = corpus.hex64(tag << 56 | LOW56 - 0xFFF)
        initial[f"x{number}"] = corpus.hex64((address + 0x1000) >> shift)
        initial["tbi_enabled"] = corpus.hex64(1)
        return words, case
    return None


def equivalence_records(records: list[dict]) -> tuple[list[dict], dict]:
    """The arm64-tbi-equivalence records for a dump's records, and their
    counts."""
    sources = []
    for record in records:
        if "4k" not in record["profiles"]:
            continue
        sources.extend(source for case in record["cases"]
                       if (source := as_source(record, case)) is not None)
    groups: dict[tuple, dict] = collections.OrderedDict()
    counts: collections.Counter = collections.Counter()
    taken: collections.Counter = collections.Counter()
    names: set[tuple] = set()

    def add(source: Source, variant: str) -> bool:
        options = tuple(source.record["options"])
        svl = source.record.get("svl", 0)
        tag = TAGS[counts["cases"] % len(TAGS)]
        derived = derive(source, variant, tag, TAGS[(counts["cases"] + 3) %
                                                    len(TAGS)])
        if derived is None:
            return False
        words, case = derived
        group = groups.get((options, svl))
        if group is None:
            group = groups[(options, svl)] = {
                "record": "/".join(
                    [EQUIVALENCE, "4k"] + (["+".join(options)] if options
                                           else []) +
                    ([f"svl={svl}"] if svl else [])),
                "producer": EQUIVALENCE, "site": SITE, "profiles": ["4k"],
                "options": list(options), "programs": [], "cases": [],
                "index": {}}
            if svl:
                group["svl"] = svl
        program = {"entry": 0, "words": [[4 * offset, f"{word:08x}"]
                                         for offset, word in
                                         enumerate(words)]}
        key = json.dumps(program, sort_keys=True)
        case["program"] = group["index"].setdefault(
            key, len(group["programs"]))
        if case["program"] == len(group["programs"]):
            group["programs"].append(program)
        # Two sources of one name, in different records of one group, take
        # a number.
        stem = case["name"]
        number = 1
        while (group["record"], case["name"]) in names:
            number += 1
            case["name"] = f"{stem} #{number}"
        names.add((group["record"], case["name"]))
        group["cases"].append(case)
        counts["cases"] += 1
        counts[variant] += 1
        return True

    for source in select(sources):
        add(source, "base")
    # Variants come from any source, since they need one of a particular shape.
    for source in sources:
        form = source.forms[0]
        # A register-offset form varies by extension and scaling; the others
        # by which of three ranges their base register is in.
        extra = source.words[0] >> 12 & 15 if form.kind == "register" \
            else form.base // 11
        for variant in VARIANTS:
            key = (variant, form.kind, form.label, extra)
            if taken[key] < PER_VARIANT and add(source, variant):
                taken[key] += 1
    result = []
    for group in groups.values():
        del group["index"]
        result.append(group)
    summary = {
        "cases": counts["cases"], "base_cases": counts["base"],
        "tbi_off_cases": counts["control"],
        "sp_cases": counts["sp"] + counts["sp control"],
        "index_cases": counts["index"] + counts["carry"],
    }
    return result, summary


# Sections, in the order their records are written.
SECTIONS = (equivalence_records,)


# --------------------------------------------------------------------------
# Record assembly


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dump", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    options = parser.parse_args()
    lines = options.dump.read_text(encoding="utf-8").splitlines()
    header = json.loads(lines[0])
    kept = []
    sources = []
    for line in lines[1:]:
        if not line.strip():
            continue
        if line.startswith('{"cases"'):
            record = json.loads(line)
            if record["producer"] == EQUIVALENCE:
                continue
            sources.append(record)
        kept.append(line)
    records, summary = equivalence_records(sources)
    header["meta"]["tbi_memory"] = {
        "script": SITE,
        "sha256": hashlib.sha256(pathlib.Path(__file__).read_bytes())
        .hexdigest(), "equivalence": summary,
    }
    with options.output.open("w", encoding="utf-8") as sink:
        sink.write(json.dumps(header, sort_keys=True) + "\n")
        for line in kept:
            sink.write(line + "\n")
        for record in records:
            sink.write(json.dumps(record, sort_keys=True) + "\n")
    print(f"appended {summary['cases']} top-byte-ignore equivalence cases "
          f"({summary['tbi_off_cases']} with TBI off, {summary['sp_cases']} "
          f"on SP, {summary['index_cases']} with the tag in the index)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
