#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the A64 instruction corpus's SM = 0 vector-length records.

A virtual CPU with FEAT_SME and without FEAT_SVE executes CNT[BHWD], RDVL,
ADDVL, ADDPL and the scalar INC[BHWD] and DEC[BHWD] outside streaming mode
for a constant vector length of 128 bits, a deliberate departure from the
architecture (darwin/cpu/arm64/detail/sm0_vector_length.hpp). An M4 traps
those words, so their results are the project's, and
tools/arm64-tests/sm0-vl-corpus.py writes them from the instructions'
pseudocode. This test re-derives the contract from the current script and
encoding table:

- provenance.json names the script and the table, and its case count;
- the one arm64-sm0-vl record, under without_sve, is the script's, and
  every row of the family has a case in it;
- the model agrees with values written out here by hand;
- no arm64-sme-native case holds a family word at PSTATE.SM = 0, whose
  results are the M4's trap; and
- the family words are undefined only where neither profile is: they are
  in the arm64-absent-sve record under without_sme and without_sve, and in
  no other record that expects them undefined, so no other SVE word has
  moved.

build-unit-corpus.py --check already refuses a pack whose recorded script or
table hash is stale.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import importlib.util
import json
import pathlib
import sys

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
GENERATOR = REPOSITORY / "tools" / "arm64-tests" / "sm0-vl-corpus.py"


def load_generator():
    spec = importlib.util.spec_from_file_location("sm0_vl_corpus", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def registers_after(gen, word: int) -> dict[int, int]:
    """The registers a word changes, by number (31 is SP)."""
    after = gen.execute(word)
    return {number: value for number, value in enumerate(after.value)
            if value != gen.corpus.FILL_REGISTERS[number]}


def check_model(gen) -> None:
    fill = gen.corpus.FILL_REGISTERS
    mask = (1 << 64) - 1
    # CNTB X0 (ALL) is 16, CNTD X1 is 2, CNTW X2, VL3, MUL #2 is 6, a
    # pattern that counts more than there is counts nothing, MUL3 of 16
    # bytes is 15, and a reserved pattern counts nothing.
    expectations = [
        (gen.cnt(0, 31, 1, 0), {0: 16}),
        (gen.cnt(3, 31, 1, 1), {1: 2}),
        (gen.cnt(2, 3, 2, 2), {2: 6}),
        (gen.cnt(3, 3, 1, 3), {3: 0}),  # VL3 of two doublewords
        (gen.cnt(0, 30, 1, 4), {4: 15}),  # MUL3
        (gen.cnt(0, 29, 16, 5), {5: 256}),  # MUL4, MUL #16
        (gen.cnt(1, 0, 1, 6), {6: 8}),  # POW2 of eight halfwords
        (gen.cnt(2, 0, 1, 7), {7: 4}),
        (gen.cnt(0, 14, 1, 8), {8: 0}),  # a reserved pattern
        (gen.cnt(0, 9, 1, 9), {9: 16}),  # VL16
        (gen.cnt(0, 10, 1, 10), {10: 0}),  # VL32 of 16 bytes
        (gen.inc_dec(False, 0, 31, 1, 11), {11: (fill[11] + 16) & mask}),
        (gen.inc_dec(True, 3, 31, 3, 12), {12: (fill[12] - 6) & mask}),
        (gen.inc_dec(False, 0, 31, 1, 31), {}),  # XZR discards
        (gen.inc_dec(False, 0, 14, 1, 13), {}),  # a reserved pattern
        (gen.rdvl(1, 14), {14: 16}),
        (gen.rdvl(-32, 15), {15: (-512) & mask}),
        (gen.rdvl(31, 31), {}),  # XZR discards
        (gen.add_length(False, 1, 2, 3), {1: (fill[2] + 48) & mask}),
        (gen.add_length(True, 1, 2, 3), {1: (fill[2] + 6) & mask}),
        (gen.add_length(False, 31, 31, -1), {31: (fill[31] - 16) & mask}),
        (gen.add_length(True, 5, 31, 31), {5: (fill[31] + 62) & mask}),
    ]
    for word, changed in expectations:
        if registers_after(gen, word) != changed:
            raise SystemExit(f"{word:08x}: the script's model differs from "
                             "the hand-written expectation")


def check_record(gen, record: dict, meta: dict) -> int:
    item = gen.record()
    if record != item:
        raise SystemExit(f"{record['record']}: not the script's record")
    if record["options"] != [gen.WITHOUT_SVE] or record["svl"] != gen.SVL:
        raise SystemExit(f"{record['record']}: not under without_sve at "
                         "the profile's SVL")
    if (meta.get("script"), meta.get("cases"), meta.get("vector_length")) != \
            (gen.SITE, len(item["cases"]), gen.VL) or \
            meta.get("sha256") != hashlib.sha256(
                GENERATOR.read_bytes()).hexdigest() or \
            meta.get("table", {}).get("sha256") != gen.table().sha256:
        raise SystemExit("provenance.json describes another script or table")
    covered = set()
    for case in record["cases"]:
        program = record["programs"][case["program"]]
        word = int(program["words"][0][1], 16)
        if not gen.sme_table.sm0_vector_length_word(word) or \
                case["event"] != "system-call" or case["retired"] != 2:
            raise SystemExit(f"{case['name']}: not a completed family word")
        covered.add(gen.table().row_for(word).witness)
    family = {row.witness for row in gen.table().rows
              if gen.sme_table.sm0_vector_length_word(row.witness)}
    if len(family) != 15 or covered != family:
        raise SystemExit("the record does not cover every row of the family")
    return len(record["cases"])


def check_others(gen, records: list[dict]) -> None:
    table = gen.sme_table
    for item in records:
        if item["producer"] == gen.PRODUCER:
            continue
        for case in item["cases"]:
            program = item["programs"][case["program"]]
            word = int(program["words"][0][1], 16)
            if not table.sm0_vector_length_word(word):
                continue
            streaming = int(case["initial"].get("svcr", "0x0"), 16) & 1
            undefined = case["event"] == "illegal-instruction"
            where = f"{item['record']}: {case['name']}"
            if item["producer"] == "arm64-sme-native" and not streaming:
                raise SystemExit(f"{where}: an M4 result for a word the "
                                 "virtual CPU executes")
            if undefined and not streaming and (
                    item["producer"] != "arm64-absent-sve" or
                    item["options"] != ["without_sme", "without_sve"]):
                raise SystemExit(f"{where}: expects a family word undefined "
                                 "at SM = 0 where SME may be implemented")


def main() -> int:
    gen = load_generator()
    builder = gen.corpus
    corpus, tokens = builder.decode_pack(pathlib.Path(sys.argv[1]).read_bytes())
    records = [item for item in map(json.loads,
                                    builder.dump_corpus(corpus, tokens))
               if "cases" in item]
    check_model(gen)
    ours = [item for item in records if item["producer"] == gen.PRODUCER]
    if len(ours) != 1:
        raise SystemExit(f"{len(ours)} arm64-sm0-vl records, expected one")
    cases = check_record(gen, ours[0], corpus.meta.get("sm0_vl", {}))
    check_others(gen, records)
    print(f"SM = 0 vector-length records: {cases} project-defined cases "
          "cover the family's 15 rows")
    return 0


if __name__ == "__main__":
    sys.exit(main())
