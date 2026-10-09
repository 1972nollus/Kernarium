#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the A64 instruction corpus's SVE FEAT_AFP records and their M4
request.

tools/arm64-tests/sve-afp-corpus.py authors the arm64-sve-afp records: the
SVE floating-point forms under FPCR.AH, FIZ, and NEP at VL 16, from M4
streaming runs with every 128-bit segment holding the case. This test
checks, whether or not the M4 has answered yet:

- every case's form is a floating-point form legal in streaming mode,
  neither a reduction nor a conversion from integers, so that its VL 16
  result is the M4's low segment;
- every case sets AH or FIZ, starts with FPSR clear and predicates all
  true, and names its row's non-streaming requirement and afp, plus rpres
  for single-precision FRECPE and FRSQRTE under AH;
- the pack's records, when it has any, answer the script's request case
  by case, as provenance.json says; and
- the M4 path records a response whose segments agree, and refuses one
  whose segments differ.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import dataclasses
import hashlib
import importlib.util
import io
import json
import pathlib
import struct
import sys

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
GENERATOR = REPOSITORY / "tools" / "arm64-tests" / "sve-afp-corpus.py"


def load_generator():
    spec = importlib.util.spec_from_file_location("sve_afp_corpus",
                                                  GENERATOR)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def response(gen, finals) -> bytes:
    """An M4 response giving each case of the request the final state
    `finals` returns for it."""
    sme = gen.sme_native
    out = bytearray(sme.RESPONSE_MAGIC)
    features = sum(1 << sme.HOST_FEATURES.index(name)
                   for name in ("FEAT_SME", "FEAT_SME2"))
    out += struct.pack("<IIII", sme.RESPONSE_VERSION, gen.SVL, features,
                       len(gen.drafts()))
    for draft in gen.drafts():
        out += struct.pack("<IIQI", 0, 4, 0, 0)
        sme._put_state(out, finals(draft), gen.SVL)
    return bytes(out)


def check_drafts(gen) -> None:
    native = gen.native
    for draft in gen.drafts():
        row = native.row_index().row_for(draft.words[0])
        if row is None or not gen.selected(row) or len(draft.words) != 1:
            raise SystemExit(f"{draft.name}: not a selected form")
        if not draft.fpcr & (gen.AH | gen.FIZ) or draft.fpsr or \
                any(draft.p[number] != native.ALL_TRUE for number in
                    native.predicate_operands(row, draft.words[0])):
            raise SystemExit(f"{draft.name}: not an FEAT_AFP case")
        needs = set(row.non_streaming_requirement) | {"afp"}
        if draft.fpcr & gen.AH and row.encoding.startswith(gen.ESTIMATES) \
                and native.source_format(row, draft.words[0])[1] == \
                native.SINGLE:
            needs.add("rpres")
        if set(draft.native) != needs:
            raise SystemExit(f"{draft.name}: native {draft.native}")


def check_m4_path(gen) -> None:
    cases = [gen.sve_sme.m4_case(draft) for draft in gen.drafts()]
    _host, results = gen.sme_native.read_response(
        response(gen, lambda draft: gen.sve_sme.m4_case(draft).state),
        cases, gen.SVL)
    if len(gen.items_of(results)) != len(gen.drafts()):
        raise SystemExit("the M4 path does not record every case")
    first = gen.drafts()[0]

    def broken(draft):
        state = gen.sve_sme.m4_case(draft).state
        if draft is first:
            z = dict(state.z)
            z[0] = z[0][:-1] + bytes([z[0][-1] ^ 1])
            return dataclasses.replace(state, z=z)
        return state

    _host, results = gen.sme_native.read_response(response(gen, broken),
                                                  cases, gen.SVL)
    errors = io.StringIO()
    try:
        with contextlib.redirect_stderr(errors):
            gen.items_of(results)
    except SystemExit as error:
        if "1 M4 cases disagree" not in str(error) or \
                f"{first.name}: the segments of z0 differ" not in \
                errors.getvalue():
            raise SystemExit(f"the M4 path refuses for another reason: "
                             f"{error}") from error
    else:
        raise SystemExit("the M4 path accepts differing segments")


def main() -> int:
    gen = load_generator()
    builder = gen.corpus
    pack = pathlib.Path(sys.argv[1])
    corpus, tokens = builder.decode_pack(pack.read_bytes())
    records = [item for item in map(json.loads,
                                    builder.dump_corpus(corpus, tokens))
               if item.get("producer") == gen.PRODUCER]
    meta = corpus.meta.get("sve_afp")
    check_drafts(gen)
    if bool(records) != (meta is not None):
        raise SystemExit("the pack's SVE FEAT_AFP records and their "
                         "provenance disagree")
    if records:
        if meta["request_sha256"] != \
                hashlib.sha256(gen.request()).hexdigest():
            raise SystemExit("the pack's records answer another request")
        if [record["record"] for record in records] != \
                [f"{gen.PRODUCER}/{gen.GROUP}"] or \
                (records[0].get("vl"), records[0].get("svl", 0),
                 records[0]["options"]) != (gen.VL, 0, []):
            raise SystemExit("the records are not the script's")
        if [case["name"] for case in records[0]["cases"]] != \
                [draft.name for draft in gen.drafts()] or \
                [sorted(case["native"]) for case in records[0]["cases"]] \
                != [sorted(draft.native) for draft in gen.drafts()]:
            raise SystemExit("the record does not answer the request")
    check_m4_path(gen)
    print(f"SVE FEAT_AFP request: {len(gen.drafts())} cases of "
          f"{len(gen.rows())} rows; "
          + (f"{len(records[0]['cases'])} recorded" if records
             else "the records await the M4"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
