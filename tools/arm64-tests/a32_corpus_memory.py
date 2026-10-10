#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The memory suites of the AArch32 instruction corpus.

A load or store case names the address it wants; the planner solves the base
(and, for a register offset, chooses the index register) so the instruction's
effective address is exactly that, whatever the offset and shift. The
addresses probe what a memory system can get wrong: aligned and misaligned
accesses, accesses that cross a 4 KiB and a 16 KiB line, the last bytes of the
data region, the first byte of the read-only region, unmapped memory and
address zero.

- memory: single-register loads and stores and the multiple-register forms,
  at addresses an AArch32 process may use on any host, and the unaligned
  accesses a single-register form permits;
- unaligned: the multi-word forms at unaligned addresses, which qemu-arm
  rejects with an alignment fault and an arm64 Linux kernel fixes up for a
  compat task, so only hardware records settle them (decision D3);
- exclusive: LDREX/STREX sequences, with CLREX, with a lone STREX and across
  addresses.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re

from a32_corpus_plan import (  # noqa: E402
    FLAGS, ISA_LETTER, Case, Context, candidate_words, corpus, fields_of, get_field, pattern,
    set_field, usable, word_of,
)

MASK = 0xFFFFFFFF
D = corpus.DATA_BASE
READ_ONLY = corpus.READ_ONLY_BASE
INDEX_VALUES = (0x10, 4, 0x80000000, 0xFFFFFFF0, 1, 0x20, 0x7FFFFFFF, 3)
SIZED = re.compile(r"(LDR|STR|LDA|STL)(EX)?(SB|SH|B|H|D)?T?\Z")
MULTIPLE = re.compile(r"(LDM|STM)(DA|DB|IA|IB)?\Z|(PUSH|POP)\Z")
SIZE_OF = {None: 4, "B": 1, "H": 2, "D": 8, "SB": 1, "SH": 2}
RECIPES = ("edges", "pattern", "edges5", "pattern1", "edges3", "edges8")


def mnemonic(name: str) -> str:
    return name.split("_")[0]


def access_size(name: str):
    match = SIZED.fullmatch(mnemonic(name))
    return SIZE_OF[match.group(3)] if match else None


def multiple_mode(name: str) -> str:
    """IA, IB, DA or DB: where the transfer lies relative to the base."""
    text = mnemonic(name)
    if text == "PUSH":
        return "DB"
    if text == "POP":
        return "IA"
    return text[3:] or "IA"


def shift_value(value: int, kind: str, amount: int, carry: int) -> int:
    kind = kind.removeprefix("SRType_")
    value &= MASK
    if kind == "LSL":
        return value << amount & MASK
    if kind == "LSR":
        return value >> amount if amount < 32 else 0
    if kind == "ASR":
        signed = value - (1 << 32) if value >> 31 else value
        return signed >> min(amount, 31) & MASK
    if kind == "ROR":
        amount %= 32
        return (value >> amount | value << (32 - amount)) & MASK
    return (carry << 31 | value >> 1) & MASK  # RRX


def register_form(operands: dict) -> bool:
    return operands.get("register_form", "m" in operands and "imm32" not in operands)


def solve(operands: dict, address: int, apsr: int, choice: int):
    """Register assignments that make a single access's effective address
    `address`, or None when the instruction cannot be aimed (PC or a clashing
    index register)."""
    n = operands["n"]
    if n == 15:
        return None
    assignments = {}
    if register_form(operands):
        m = operands["m"]
        if m == 15 or m == n:
            return None
        value = INDEX_VALUES[choice % len(INDEX_VALUES)]
        assignments[f"r{m}"] = value
        offset = shift_value(value, operands.get("shift_t", "SRType_LSL"),
                             operands.get("shift_n", 0), apsr >> 29 & 1)
    else:
        offset = operands.get("imm32", 0) & MASK  # LDA, STL and the exclusives have none
    signed = offset if operands.get("add", True) else -offset
    index = operands.get("index", not operands.get("postindex", False))
    assignments[f"r{n}"] = (address - signed if index else address) & MASK
    return assignments


def solve_multiple(operands: dict, mode: str, start: int):
    """The base register value for a transfer whose lowest address is `start`."""
    count = bin(operands["registers"]).count("1")
    base = {"IA": start, "IB": start - 4, "DA": start + 4 * count - 4, "DB": start + 4 * count}[mode]
    return {f"r{operands.get('n', 13)}": base & MASK}


def single_scenarios(size: int):
    return {
        "a": D + 0x1040, "x4": D + 0x2000 - size // 2, "x16": D + 0x4000 - size // 2,
        "last": D + 0x8000 - size, "ro": READ_ONLY, "unmapped": 0x60000000, "zero": 0,
        "wrap": 0xFFFFFFFC, "u1": D + 0x1041, "u2": D + 0x1042, "u3": D + 0x1043,
    }


def multiple_scenarios(count: int):
    half = 4 * (count // 2)
    return {
        "a": D + 0x1040, "x4": D + 0x2000 - half, "x16": D + 0x4000 - half,
        "last": D + 0x8000 - 4 * count, "ro": READ_ONLY, "unmapped": 0x60000000, "zero": 0,
        "u1": D + 0x1041, "u2": D + 0x1042, "u3": D + 0x1043,
    }


def memory_forms(context: Context, level: str):
    for form in context.model.forms:
        if (form.encoding is None or form.level != level or form.availability != "implemented"
                or form.check or form.nop or "exclusive" in form.traits or "prefetch" in form.traits
                or not form.traits & {"loads", "stores"} or "branch" in form.traits):
            continue
        if MULTIPLE.fullmatch(mnemonic(form.name)) or access_size(form.name) is not None:
            yield form


def is_wide(form_name: str) -> bool:
    return MULTIPLE.fullmatch(mnemonic(form_name)) is not None or access_size(form_name) == 8


def pick_words(context, form, count=6):
    words = []
    for word in candidate_words(context, form, 8):
        found = usable(context, form, word)
        if found is not None and found[0] not in [w for w, _ in words]:
            words.append(found)
        if len(words) == count:
            break
    return words


def single_case(context, suite, form, index, word, operands, scenario, address, number):
    isa = form.encoding.isa
    suffix = "a" if isa == "A32" else "t"
    recipe = f"{RECIPES[(index + number) % len(RECIPES)]}-{suffix}"
    flags = FLAGS[pattern(number) >> 8 & 3]
    if "n" not in operands or operands["n"] == 15:  # literal: the address is the program's own
        return Case(suite, f"{form.name}/{index}.lit", form.name, ISA_LETTER[isa],
                    word_of(isa, word), 0, recipe, {"apsr": flags})
    aim = solve(operands, address, flags, number)
    if aim is None:
        return None
    return Case(suite, f"{form.name}/{index}.{scenario}", form.name, ISA_LETTER[isa],
                word_of(isa, word), 0, recipe, {"apsr": flags, **aim})


def multiple_case(context, suite, form, index, word, operands, scenario, start, number):
    isa = form.encoding.isa
    suffix = "a" if isa == "A32" else "t"
    recipe = f"{RECIPES[(index + number) % len(RECIPES)]}-{suffix}"
    flags = FLAGS[pattern(number) >> 8 & 3]
    aim = solve_multiple(operands, multiple_mode(form.name), start)
    return Case(suite, f"{form.name}/{index}.{scenario}", form.name, ISA_LETTER[isa],
                word_of(isa, word), 0, recipe, {"apsr": flags, **aim})


def build(context: Context, level: str, suite: str, ordinary: bool) -> list[Case]:
    """ordinary: the memory suite; otherwise the unaligned multi-word forms."""
    cases = []
    used = set()
    for form_index, form in enumerate(memory_forms(context, level)):
        wide = is_wide(form.name)
        multiple = MULTIPLE.fullmatch(mnemonic(form.name)) is not None
        size = access_size(form.name)
        names = (["a", "x4", "x16", "last", "ro", "unmapped", "zero", "wrap"] if ordinary else [])
        if multiple:
            names = [name for name in names if name != "wrap"]
        if ordinary and not wide and size >= 2:
            names += ["u1"] + (["u2", "u3"] if size == 4 else [])
        if not ordinary:
            if not wide:
                continue
            names = ["u1", "u2", "u3"]
        for index, (word, operands) in enumerate(pick_words(context, form)):
            for step in range(2 if ordinary else 1):
                number = len(cases) + form_index
                scenario = names[(index * 2 + step + form_index) % len(names)]
                if step == 0 and ordinary:
                    scenario = "a" if index % 2 == 0 else scenario
                if multiple:
                    count = bin(operands["registers"]).count("1")
                    start = multiple_scenarios(count)[scenario]
                    case = multiple_case(context, suite, form, index, word, operands, scenario,
                                         start, number)
                else:
                    address = single_scenarios(size)[scenario]
                    case = single_case(context, suite, form, index, word, operands, scenario,
                                       address, number)
                if case is not None and case.name not in used:
                    used.add(case.name)
                    cases.append(case)
    return cases


def suite_memory(context: Context) -> list[Case]:
    return build(context, "base", "memory", True)


def suite_unaligned(context: Context) -> list[Case]:
    return build(context, "base", "unaligned", False)


def suite_prefetch(context: Context) -> list[Case]:
    """PLD, PLDW and PLI never fault, whatever the address names."""
    cases = []
    used = set()
    names = ["a", "unmapped", "zero", "wrap", "ro", "x4"]
    forms = [form for form in context.model.forms
             if form.encoding is not None and form.level == "base" and "prefetch" in form.traits
             and form.availability == "implemented"]
    for form_index, form in enumerate(forms):
        for index, (word, operands) in enumerate(pick_words(context, form, 4)):
            for step in range(2):
                scenario = names[(index + 2 * step + form_index) % len(names)]
                case = single_case(context, "prefetch", form, index, word, operands, scenario,
                                   single_scenarios(1)[scenario], len(cases) + form_index)
                if case is not None and case.name not in used:
                    used.add(case.name)
                    cases.append(case)
    return cases


# Exclusives -----------------------------------------------------------------------------------------------

EXCLUSIVE_SIZES = ("", "B", "H", "D")
CLREX = {"A": [0xF57FF01F], "T": [0xF3BF, 0x8F2F]}


def exclusive_word(context, name: str, **registers):
    """A decodable word of the form with the named register fields set."""
    form = context.model.by_name[name]
    fields = fields_of(form)
    for word in candidate_words(context, form, 12):
        found = usable(context, form, word, True)
        if found is None:
            continue
        word = found[0]
        for field, value in registers.items():
            if field in fields:
                word = set_field(word, *fields[field], value)
        again = usable(context, form, word, True)
        if again is not None:
            return again
    return None


def program_units(word: int, isa: str) -> list[int]:
    """One instruction as program units: its word in A32, its halfwords in Thumb."""
    if isa == "A32":
        return [word]
    return [word >> 16] if isa == "T16" else [word >> 16, word & 0xFFFF]


def exclusive_cases(context: Context, suite: str, load_stem: str, store_stem: str) -> list[Case]:
    cases = []
    for state, suffix in (("A", "A1"), ("T", "T1")):
        letter = "a" if state == "A" else "t"
        for size_name in EXCLUSIVE_SIZES:
            size = {"": 4, "B": 1, "H": 2, "D": 8}[size_name]
            load_name, store_name = f"{load_stem}{size_name}_{suffix}", f"{store_stem}{size_name}_{suffix}"
            wide = size_name == "D"
            pair = {"Rt2": 3} if wide else {}  # Rt2 is SBO in the byte and halfword forms
            loads = exclusive_word(context, load_name, Rn=4, Rt=2 if wide else 1, **pair)
            stores = exclusive_word(context, store_name, Rn=4, Rd=5, Rt=6, **({"Rt2": 7} if wide else {}))
            isa = context.model.by_name[load_name].encoding.isa
            ldrex, strex = program_units(loads[0], isa), program_units(stores[0], isa)
            if state == "A":
                ldrex, strex = [set_field(ldrex[0], 28, 4, 14)], [set_field(strex[0], 28, 4, 14)]
            clrex = CLREX[state]

            def aimed(address, choice):
                if "imm32" in loads[1]:
                    return solve({**loads[1], "n": 4}, address, 0, choice)
                return {"r4": address}

            shapes = {"pair": [ldrex, strex], "lone": [strex], "clrex": [ldrex, clrex, strex],
                      "twice": [ldrex, ldrex, strex]}
            for index, (scenario, address) in enumerate((("a", D + 0x1040), ("x4", D + 0x2000 - size),
                                                          ("last", D + 0x8000 - size))):
                aim = aimed(address, index)
                for shape, parts in shapes.items():
                    units = [unit for part in parts for unit in part]
                    target = [i for i, part in enumerate(parts) if part is strex][0]
                    recipe = f"{('edges', 'pattern')[index % 2]}-{letter}"
                    cases.append(Case(suite, f"{store_name}/{shape}.{scenario}", store_name, state,
                                      tuple(units), target, recipe, {**aim, "r5": 9}))
            # A load that faults: unmapped, or misaligned (an alignment fault, which
            # no kernel fixes up for an exclusive).
            faulting = {"unmapped": 0x60000000, "zero": 0}
            if size >= 2:
                faulting.update(u1=D + 0x1041, u2=D + 0x1042)
            if size == 8:
                faulting.update(u4=D + 0x1044)
            if size == 4:
                faulting.update(u3=D + 0x1043)
            for scenario, address in faulting.items():
                cases.append(Case(suite, f"{load_name}/load.{scenario}", load_name, state,
                                  tuple(ldrex), 0, f"edges-{letter}", aimed(address, 0)))
    return cases


def suite_exclusive(context: Context) -> list[Case]:
    """LDREX/STREX and the acquire/release LDAEX/STLEX, which are base too."""
    return (exclusive_cases(context, "exclusive", "LDREX", "STREX")
            + exclusive_cases(context, "exclusive", "LDAEX", "STLEX"))


SUITES = {"memory": suite_memory, "unaligned": suite_unaligned, "exclusive": suite_exclusive,
          "prefetch": suite_prefetch}
