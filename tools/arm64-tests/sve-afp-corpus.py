#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Author the corpus cases of the SVE floating-point forms under FEAT_AFP's
FPCR controls outside streaming mode.

FPCR.AH, FIZ, and NEP need FEAT_AFP. arm64-builder's Neoverse V2 lacks it, so
the native SVE records sample only the rounding mode, FZ, DN, and FZ16. The
M4 has FEAT_AFP but no SVE, so it executes these forms only in streaming
mode. Each form here is lane-local within a 128-bit segment, so the M4 runs
each case in streaming mode at VL 64 with every Z and P register holding its
VL 16 value in each segment, as sve-sme-corpus.py does. Every segment then
computes the VL 16 result and raises the VL 16 flags, so the response's low
segment and its FPSR are the VL 16 outcome. The script requires every
segment of every register to agree, which also refuses a form that is not
lane-local.

The forms are the floating-point rows of sve-native-corpus.py's fpcr group,
at each element size, that the M4 executes in streaming mode. Three kinds
are left out: the reductions (FADDV, FMAXV, FMINV, FMAXNMV, and FMINNMV;
FADDA is not legal in streaming mode), whose results depend on the vector
length; the conversions from integers, which no FEAT_AFP control affects;
and the forms legal only outside streaming mode. Each case holds one operand
class in every source, with the class's FPCR samples:

- subnormals, which FIZ flushes on input and AH changes the flushing of:
  FIZ alone, AH, AH with FIZ, AH with FIZ and NEP, and AH with FZ, FZ16, and
  DN;
- NaNs, whose propagation AH changes: AH, and AH with FZ, FZ16, and DN;
- signed zeros, which AH changes FMAX and FMIN for: AH;
- ties, which the BF16 forms round to nearest under AH whatever the
  rounding mode: AH with RZ, which every other form follows;
- subnormals beside NaNs, in each order and, where a form has a third
  source, beside a one: AH reports a subnormal operand (IDC) only when no
  operand is a NaN, so a subnormal beside a NaN differs from a subnormal
  beside a number. FIZ alone, AH, AH with FIZ, and AH with FZ, FZ16, and DN.

NEP affects only AdvSIMD scalar forms, so one sample pins it as a no-op.
FPCR must read back as the case set it, so a host whose FPCR holds AH, FIZ
and NEP as zero, one without FEAT_AFP, records nothing. The M4 also has
FEAT_RPRES, which the estimates under AH need; the oracle does not report
it.
Predicates are all true, and FPSR starts clear. Each case names its row's
non-streaming requirement and afp; the single-precision FRECPE and FRSQRTE
cases under AH name rpres as well, which their estimates' precision needs.
The runner runs them only for an engine at sve_support complete on a
virtual CPU with FEAT_AFP. The records hold for both page profiles at VL 16
without an SVL, in arm64-sve-afp/m4.

    sve-afp-corpus.py --m4-request afp.request
    sme_native_oracle afp.request afp.response.1        (twice, on the M4)
    sve-afp-corpus.py sve-sme.jsonl sve-afp.jsonl \\
        --m4-response afp.response.1 --m4-response afp.response.2
    sve-afp-corpus.py sve-sme.jsonl sve-afp.jsonl --carry PACK

The input is the dump after the SVE/SME step. --carry PACK copies the
records of a pack whose request is unchanged, or none if it has none.
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


def _load(name: str, path: pathlib.Path):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


native = _load("sve_native_corpus", HERE / "sve-native-corpus.py")
sve_sme = _load("sve_sme_corpus", HERE / "sve-sme-corpus.py")
corpus = native.corpus
sme_native = native.sme_native
sme_table = native.sme_table

SITE = "tools/arm64-tests/sve-afp-corpus.py"
PRODUCER = "arm64-sve-afp"
GROUP = "m4"
VL = native.VL
SVL = sve_sme.SVL
FIZ, AH, NEP = 1 << 0, 1 << 1, 1 << 2
FLUSH_DN = 1 << native.FPCR_FZ | 1 << native.FPCR_FZ16 | \
    1 << native.FPCR_DN
MIXED_SAMPLES = (FIZ, AH, AH | FIZ, AH | FLUSH_DN)
CLASS_SAMPLES = {
    "subnormals": (FIZ, AH, AH | FIZ, AH | FIZ | NEP, AH | FLUSH_DN),
    "NaNs": (AH, AH | FLUSH_DN),
    "zeros": (AH,),
    "ties": (AH | 3 << native.FPCR_RMODE,),
}
# Which of a form's first sources hold a subnormal (S), a NaN (N) or a one
# (O) in a mixed case, by the number of sources the form has: each order of
# a subnormal and a NaN, with the third source a one.
MIXED_ORDERS = {2: ("SN", "NS"),
                3: ("SNO", "SON", "NSO", "NOS", "OSN", "ONS")}
REDUCTIONS = ("faddv_", "fmaxv_", "fminv_", "fmaxnmv_", "fminnmv_",
              "fadda_")
# The features beside SME that a streaming row may need, which the M4 has.
M4_FEATURES = {"sme", "bf16"}
ESTIMATES = ("frecpe_", "frsqrte_")


def selected(row) -> bool:
    """Whether a row is a floating-point form that FEAT_AFP's controls
    affect, that the M4 executes in streaming mode, and that is
    lane-local."""
    if not native.floating(row) or row.encoding.startswith(REDUCTIONS) or \
            row.encoding.startswith(("scvtf", "ucvtf")):
        return False
    if "sme" not in row.requirement:
        return False
    if not set(row.requirement) <= M4_FEATURES:
        raise SystemExit(f"{row.encoding}: needs {row.requirement} in "
                         "streaming mode, which the M4 may lack")
    return True


@functools.cache
def rows() -> tuple:
    return tuple(row for row in native.sve_rows() if selected(row))


def classes(form: tuple[int, int]) -> tuple[tuple[str, list], ...]:
    """sve-native-corpus.py's classes but overflow, and signed zeros."""
    exponent, fraction = form
    sign = 1 << exponent + fraction
    one = ((1 << exponent - 1) - 1) << fraction
    zeros = [0, sign, sign, 0, one, sign, 0, one | sign]
    return tuple(item for item in native.float_classes(form)
                 if item[0] != "overflow") + (("zeros", zeros),)


def mixed(form: tuple[int, int]) -> dict[str, list]:
    """The lanes of a mixed case's subnormals, NaNs (quiet and signalling,
    both signs) and ones."""
    exponent, fraction = form
    sign = 1 << exponent + fraction
    top = ((1 << exponent) - 1) << fraction
    one = ((1 << exponent - 1) - 1) << fraction
    quiet = 1 << fraction - 1
    tiny, widest = 1, (1 << fraction) - 1
    return {
        "S": [tiny, widest | sign, tiny | sign, widest, 3 * tiny, tiny,
              widest | sign, tiny | sign],
        "N": [top | quiet | 1, top | 1, top | quiet | sign | 2, top | sign | 3,
              top | 1, top | quiet | 1, top | sign | 3, top | quiet | 2],
        "O": [one, one | sign, one, one | sign, one, one, one | sign, one],
    }


def requirement(row, word: int, fpcr: int) -> tuple[str, ...]:
    needs = set(row.non_streaming_requirement) | {"afp"}
    if fpcr & AH and row.encoding.startswith(ESTIMATES) and \
            native.source_format(row, word)[1] == native.SINGLE:
        needs.add("rpres")
    return tuple(sorted(needs))


@functools.cache
def drafts() -> tuple:
    """Each selected row's low word, at each element size it decodes, with
    every source holding one operand class, under the class's FPCR
    samples."""
    items = []
    for row in rows():
        low_word = native.allocate(row, row.witness,
                                   list(range(len(row.operands))), False)
        for word in native.sized_words(row, low_word):
            form = native.source_format(row, word)[1]
            bits = sum(form) + 1
            sources = [number for operand in row.operands
                       if operand.kind == "z"
                       for _kind, number in native.occupied(
                           operand, operand.number(word))]
            for label, values in classes(form):
                # Each source holds the class's values rotated by one more
                # element than the one before.
                vectors = {
                    number: b"".join(
                        values[(lane + rotation) % len(values)].to_bytes(
                            bits // 8, "little")
                        for lane in range(VL * 8 // bits))
                    for rotation, number in enumerate(sources)}
                for fpcr in CLASS_SAMPLES[label]:
                    draft = native.base_draft(
                        GROUP, row, word, f"{label}, FPCR {fpcr:#010x}")
                    draft.fpcr = fpcr
                    draft.v.update(vectors)
                    draft.native = requirement(row, word, fpcr)
                    items.append(native.with_predicates(
                        draft, row, word, native.ALL_TRUE))
            lanes = mixed(form)
            for order in MIXED_ORDERS.get(len(sources), ()):
                vectors = {
                    number: b"".join(
                        lanes[kind][lane % 8].to_bytes(bits // 8, "little")
                        for lane in range(VL * 8 // bits))
                    for number, kind in zip(sources, order)}
                for fpcr in MIXED_SAMPLES:
                    draft = native.base_draft(
                        GROUP, row, word,
                        f"mixed {order}, FPCR {fpcr:#010x}")
                    draft.fpcr = fpcr
                    draft.v.update(vectors)
                    draft.native = requirement(row, word, fpcr)
                    items.append(native.with_predicates(
                        draft, row, word, native.ALL_TRUE))
    return tuple(items)


def request() -> bytes:
    return sme_native.write_request(
        [sve_sme.m4_case(draft) for draft in drafts()], SVL)


def items_of(results: list) -> list:
    items = []
    findings = []
    for draft, result in zip(drafts(), results):
        try:
            items.append((draft, draft.wire().words,
                          sve_sme.m4_item(draft, result)))
        except native.Finding as finding:
            findings.append(str(finding))
    if findings:
        for finding in findings:
            print(f"sve-afp-corpus: {finding}", file=sys.stderr)
        raise SystemExit(f"sve-afp-corpus: {len(findings)} M4 cases "
                         "disagree")
    return items


def assemble(items: list) -> list[dict]:
    if not items:
        return []
    record = {"record": f"{PRODUCER}/{GROUP}", "producer": PRODUCER,
              "site": SITE, "profiles": list(native.BOTH), "options": [],
              "vl": VL, "programs": [], "cases": []}
    index: dict[tuple, int] = {}
    for _draft, words, item in items:
        if words not in index:
            index[words] = len(record["programs"])
            record["programs"].append({
                "entry": 0,
                "words": [[4 * i, f"{word:08x}"]
                          for i, word in enumerate(words)]})
        record["cases"].append(dict(item, program=index[words]))
    return [record]


def script_hash() -> str:
    return hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()


def meta(host: dict, cases: int) -> dict:
    return {
        "script": SITE, "sha256": script_hash(),
        "table": {"file": sme_table.TABLE_PATH,
                  "sha256": native.table().sha256},
        "vl": VL, "svl": SVL,
        "request_sha256": hashlib.sha256(request()).hexdigest(),
        "host_features": host["features"], "cases": cases,
    }


def strip(lines: list[str]) -> list[str]:
    return [line for line in lines
            if json.loads(line).get("producer") != PRODUCER]


def carried(pack_path: pathlib.Path) -> tuple[dict | None, list]:
    """The records and step metadata of a pack, which must answer this
    request."""
    pack, tokens = corpus.decode_pack(pack_path.read_bytes())
    step = pack.meta.get("sve_afp")
    records = [record for record in pack.records
               if record.producer == PRODUCER]
    if step is None and not records:
        return None, []
    if step is None or step["request_sha256"] != \
            hashlib.sha256(request()).hexdigest():
        raise SystemExit("sve-afp-corpus: the request changed since the "
                         "pack's records; run the M4 again")
    dumped = [json.loads(line) for line in corpus.dump_corpus(
        corpus.Corpus(meta=pack.meta, blobs=pack.blobs, records=records),
        tokens)]
    cases = [(item, case) for item in dumped if "cases" in item
             for case in item["cases"]]
    if len(cases) != len(drafts()):
        raise SystemExit("sve-afp-corpus: the pack's cases do not answer "
                         "the request")
    items = []
    for draft, (item, case) in zip(drafts(), cases):
        words = tuple(int(word, 16) for _offset, word in
                      item["programs"][case["program"]]["words"])
        if case["name"] != draft.name or words != draft.wire().words:
            raise SystemExit(f"sve-afp-corpus: {draft.name}: the pack's "
                             "cases do not answer the request")
        items.append((draft, words, case))
    return dict(step, sha256=script_hash()), items


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("input", type=pathlib.Path, nargs="?")
    parser.add_argument("output", type=pathlib.Path, nargs="?")
    parser.add_argument("--m4-request", type=pathlib.Path,
                        help="write the M4's request and exit")
    parser.add_argument("--m4-response", type=pathlib.Path, action="append",
                        default=[], help="an M4 response to the request; "
                        "several must give identical records")
    parser.add_argument("--carry", type=pathlib.Path,
                        help="copy the records of this pack, if any")
    options = parser.parse_args()
    if options.m4_request is not None:
        data = request()
        options.m4_request.write_bytes(data)
        print(f"wrote {len(drafts())} M4 cases, {len(data)} bytes, sha256 "
              f"{hashlib.sha256(data).hexdigest()}")
        return 0
    if options.input is None or options.output is None or \
            bool(options.m4_response) == (options.carry is not None):
        parser.error("give INPUT OUTPUT and either --m4-response or --carry")
    header, lines = sme_native.read_dump(options.input)
    lines = strip(lines)
    if options.m4_response:
        cases = [sve_sme.m4_case(draft) for draft in drafts()]
        runs = [sme_native.read_response(path.read_bytes(), cases, SVL)
                for path in options.m4_response]
        host = runs[0][0]
        if host["svl"] != SVL or "FEAT_SME" not in host["features"]:
            raise SystemExit("sve-afp-corpus: the response is not from an "
                             f"SME host with a {SVL}-byte SVL")
        built = [items_of(results) for _host, results in runs]
        if any([item for _d, _w, item in other] !=
               [item for _d, _w, item in built[0]] for other in built[1:]):
            raise SystemExit("sve-afp-corpus: the M4 responses give "
                             "different records")
        items, step = built[0], meta(host, len(cases))
    else:
        step, items = carried(options.carry)
    if step is not None:
        header["meta"]["sve_afp"] = step
    else:
        header["meta"].pop("sve_afp", None)
    records = assemble(items)
    sme_native.write_dump(options.output, header, lines, {}, records)
    print(f"appended {len(items)} SVE FEAT_AFP cases"
          if items else "no SVE FEAT_AFP records: the M4 has not answered")
    return 0


if __name__ == "__main__":
    sys.exit(main())
