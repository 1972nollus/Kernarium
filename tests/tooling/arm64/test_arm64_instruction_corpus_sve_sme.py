#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Re-derive the A64 instruction corpus's SVE/SME records.

tools/arm64-tests/sve-sme-corpus.py authors the arm64-sve-sme records: the
SME forms that SVE executes outside streaming mode, at VL 16. This test
re-derives them from the current script, the encoding table and the pack's
own native SME records:

- the models reproduce every M4 streaming case of their encodings that the
  pack holds, and provenance.json names those counts, the script, the
  table and the M4 request;
- the asl and table records are exactly the script's, without harness
  options, at VL 16 and without an SVL, in both page profiles;
- every case names its row's non-streaming requirement, which holds sve and
  sme, so the runner runs it only for an engine that implements the SVE
  profile completely on a virtual CPU with SME (and SME2 where the row
  needs it);
- an m4 record, when the pack has one, answers the script's M4 request case
  by case; and
- the M4 path accepts a response whose every segment agrees and the models
  hold, and refuses one whose segments differ or that a model contradicts.
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
GENERATOR = REPOSITORY / "tools" / "arm64-tests" / "sve-sme-corpus.py"


def load_generator():
    spec = importlib.util.spec_from_file_location("sve_sme_corpus", GENERATOR)
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
    drafts = gen.m4_drafts()
    out += struct.pack("<IIII", sme.RESPONSE_VERSION, gen.SVL, features,
                       len(drafts))
    for draft in drafts:
        out += struct.pack("<IIQI", 0, 4, 0, 0)
        sme._put_state(out, finals(draft), gen.SVL)
    return bytes(out)


def model_final(gen, draft):
    """The final state the models give a case at VL 64; an m4-family case
    leaves its state alone."""
    state = gen.m4_case(draft).state
    if draft.group != "asl":
        return state
    row = gen.native.row_index().row_for(draft.words[0])
    machine = gen.run_model(row, draft.words[0], gen.Machine(
        vl=gen.SVL, x=[state.x.get(i, gen.corpus.FILL_REGISTERS[i])
                       for i in range(32)],
        z=[state.z[i] for i in range(32)], p=[state.p[i] for i in range(16)],
        nzcv=state.nzcv))
    return dataclasses.replace(state, nzcv=machine.nzcv,
                               z=dict(enumerate(machine.z)),
                               p=dict(enumerate(machine.p)))


def check_m4_path(gen) -> None:
    cases = [gen.m4_case(draft) for draft in gen.m4_drafts()]
    host, results = gen.sme_native.read_response(
        response(gen, lambda draft: model_final(gen, draft)), cases, gen.SVL)
    items = gen.m4_items(results)
    if [draft.name for draft, _words, _item in items] != \
            [draft.name for draft in gen.drafts() if draft.group == "m4"]:
        raise SystemExit("the M4 path does not record the m4 cases")
    first = next(draft for draft in gen.m4_drafts() if draft.group == "m4")
    model = next(draft for draft in gen.m4_drafts() if draft.group == "asl")

    def broken(draft):
        state = model_final(gen, draft)
        if draft is first:
            z = dict(state.z)
            z[0] = z[0][:-1] + bytes([z[0][-1] ^ 1])
            return dataclasses.replace(state, z=z)
        if draft is model:
            return dataclasses.replace(state, nzcv=state.nzcv ^ 1 << 28)
        return state

    _host, results = gen.sme_native.read_response(response(gen, broken),
                                                  cases, gen.SVL)
    errors = io.StringIO()
    try:
        with contextlib.redirect_stderr(errors):
            gen.m4_items(results)
    except SystemExit as error:
        if "2 M4 cases disagree" not in str(error) or \
                f"{first.name}: the segments of z0 differ" not in \
                errors.getvalue() or \
                f"{model.name}: the M4 differs from the model" not in \
                errors.getvalue():
            raise SystemExit(f"the M4 path refuses for another reason: "
                             f"{error}") from error
    else:
        raise SystemExit("the M4 path accepts differing segments or a "
                         "contradicted model")


def main() -> int:
    gen = load_generator()
    builder = gen.corpus
    pack = pathlib.Path(sys.argv[1])
    corpus, tokens = builder.decode_pack(pack.read_bytes())
    lines = list(builder.dump_corpus(corpus, tokens))
    records = [item for item in map(json.loads, lines)
               if item.get("producer") == gen.PRODUCER]
    meta = corpus.meta.get("sve_sme")
    if meta is None or not records:
        raise SystemExit("the pack's SVE/SME records and their provenance "
                         "disagree")
    checked = gen.check_models(gen.strip(lines))
    if meta.get("models_checked") != dict(sorted(checked.items())):
        raise SystemExit("provenance.json names other model checks")
    if (meta.get("script"), meta.get("vl"), meta.get("m4_request_sha256")) \
            != (gen.SITE, gen.VL,
                hashlib.sha256(gen.m4_request()).hexdigest()):
        raise SystemExit("the pack's SVE/SME records name another script, VL "
                         "or M4 request")
    derived = json.loads(json.dumps(gen.assemble(gen.derived_items())))
    held = [record for record in records
            if record["record"] != f"{gen.PRODUCER}/m4"]
    if held != derived:
        raise SystemExit("the asl and table records are not the script's")
    m4 = [record for record in records
          if record["record"] == f"{gen.PRODUCER}/m4"]
    if bool(m4) != ("m4" in meta):
        raise SystemExit("the m4 record and its provenance disagree")
    if m4:
        expected = [draft for draft in gen.drafts() if draft.group == "m4"]
        if [case["name"] for case in m4[0]["cases"]] != \
                [draft.name for draft in expected]:
            raise SystemExit("the m4 record does not answer the request")
    rows = {row.encoding: row for row in gen.rows()}
    total = 0
    for record in records:
        if (record.get("vl"), record.get("svl", 0), record["options"],
                record["profiles"]) != (gen.VL, 0, [], ["16k", "4k"]):
            raise SystemExit(f"{record['record']}: not a VL 16 record of "
                             "both profiles")
        for case in record["cases"]:
            native = set(case["native"])
            row = rows[case["name"].split()[1]]
            if native != set(row.non_streaming_requirement) or \
                    not {"sve", "sme"} <= native:
                raise SystemExit(f"{case['name']}: native {case['native']}")
            total += 1
    check_m4_path(gen)
    print(f"SVE/SME records: {total} cases in {len(records)} records "
          f"re-derived from {gen.SITE}; the models reproduce "
          f"{sum(checked.values())} M4 cases; m4 record "
          f"{'present' if m4 else 'awaits the M4'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
