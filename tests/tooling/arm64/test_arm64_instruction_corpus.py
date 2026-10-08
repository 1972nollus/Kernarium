#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the A64 instruction corpus metadata and its round trips.

The pack must re-encode canonically, survive its text form, and keep every
record's meaning in pack version 2, which lacks VLs, as in version 3. A
synthetic record with every SME section and field, and one with every SVE
section and field, must survive both forms, and the encoder must refuse
state that breaks the pack's rules."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import importlib.util
import pathlib
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
BUILDER = REPOSITORY / "tools" / "arm64-tests" / "build-unit-corpus.py"
ROUND_TRIP_RECORDS = 24


def load_builder():
    spec = importlib.util.spec_from_file_location("build_unit_corpus", BUILDER)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def sme_round_trip(builder) -> None:
    """A version 2 record with every SME section and field must survive the
    binary and text forms, and the encoder must refuse SME state that breaks
    the pack's rules."""
    svl = 64
    z = lambda index: bytes((index * 7 + byte) & 0xFF for byte in range(svl))
    streaming = builder.Case(
        name="streaming", entry=0, words=((0, 0xC080_0000), (4, 0xD400_0001)),
        event="system-call", invalid_encoding=False, minimization="full",
        retired=("exact", 2), fault=None, syndrome=0, native=("sme",),
        validator=None,
        initial=((), (), (("svcr", 3), ("tpidr2", 0x1234)),
                 (((0, z(0)), (31, z(31))),
                  ((0, bytes(8)), (15, b"\xff" * 8)),
                  ((0, z(1)), (63, z(2))), z(3)[:builder.ZT0_BYTES]),
                 builder.NO_SVE),
        expected=(((1, 7),), (), (("pc", builder.CODE_BASE + 8),),
                  (((5, z(5)),), (), ((17, z(4)),), None), builder.NO_SVE),
        memory=(), ordinals=(0, 0))
    za_only = builder.Case(
        name="za only", entry=0, words=((0, 0xC080_0000), (4, 0xD400_0001)),
        event="illegal-instruction", invalid_encoding=True,
        minimization="full", retired=("exact", 0), fault=None, syndrome=0,
        native=("sme",), validator=None,
        initial=((), ((2, bytes(16)),), (("svcr", 2),),
                 ((), (), ((5, z(6)),), None), builder.NO_SVE),
        expected=((), (), (), builder.NO_SME, builder.NO_SVE), memory=(),
        ordinals=(0, 0))
    options = 1 << builder.OPTION_NAMES.index("without_sme")
    record = builder.Record(
        name="synthetic/sme", producer="synthetic", site="synthetic:1",
        options=options, profiles=3, svl=svl,
        programs=[(0, streaming.words)],
        cases=[(0, streaming), (0, za_only)])
    synthetic = builder.Corpus(meta={"producer_commit": "-"}, blobs={},
                               records=[record])
    tokens = builder.feature_tokens()
    encoded = builder.encode_pack(synthetic, tokens)
    decoded, decoded_tokens = builder.decode_pack(encoded)
    if decoded.records != synthetic.records or \
            builder.encode_pack(decoded, decoded_tokens) != encoded:
        raise SystemExit("an SME record does not survive the pack")
    reloaded, reloaded_tokens = builder.load_dump(
        builder.dump_corpus(synthetic, tokens))
    if reloaded.records != synthetic.records or \
            builder.encode_pack(reloaded, reloaded_tokens) != encoded:
        raise SystemExit("an SME record does not survive the text form")
    broken = {
        "V registers in streaming mode": dict(initial=(
            (), ((0, bytes(16)),), (("svcr", 1),), builder.NO_SME,
            builder.NO_SVE)),
        "Z outside streaming mode": dict(initial=(
            (), (), (("svcr", 2),), (((0, z(0)),), (), (), None),
            builder.NO_SVE)),
        "ZT0 while PSTATE.ZA is clear": dict(initial=(
            (), (), (("svcr", 1),), ((), (), (), z(0)[:64]),
            builder.NO_SVE)),
        "short Z": dict(initial=(
            (), (), (("svcr", 1),), (((0, z(0)[:32]),), (), (), None),
            builder.NO_SVE)),
    }
    for reason, change in broken.items():
        case = builder.dataclasses.replace(streaming, **change)
        if builder.invalid_sme_state(record, case) is None:
            raise SystemExit(f"the encoder accepts {reason}")
    plain = builder.dataclasses.replace(record, svl=0)
    if builder.invalid_sme_state(plain, streaming) is None:
        raise SystemExit("the encoder accepts SME state without an SVL")


def sve_round_trip(builder) -> None:
    """A version 3 record with every non-streaming SVE section and field
    must survive the binary and text forms; the encoder must refuse SVE
    state that breaks the pack's rules, and a version 2 pack must refuse a
    VL."""
    p = lambda value: value.to_bytes(2, "little")
    live = builder.Case(
        name="live", entry=0, words=((0, 0x2518_E3E0), (4, 0xD400_0001)),
        event="system-call", invalid_encoding=False, minimization="full",
        retired=("exact", 2), fault=None, syndrome=0, native=("sve",),
        validator=None,
        initial=((), ((3, bytes(range(16))),), (("sve_live", 1),),
                 builder.NO_SME, (((0, p(0x5555)), (15, p(0xFFFF))),
                                  p(0x00FF))),
        expected=(((2, 9),), (), (("pc", builder.CODE_BASE + 8),),
                  builder.NO_SME, (((0, p(0xFFFF)),), None)),
        memory=(), ordinals=(0, 0))
    becomes_live = builder.Case(
        name="becomes live", entry=0,
        words=((0, 0x2518_E3E0), (4, 0xD400_0001)), event="system-call",
        invalid_encoding=False, minimization="full", retired=("exact", 2),
        fault=None, syndrome=0, native=("sve",), validator=None,
        initial=((), (), (), builder.NO_SME, builder.NO_SVE),
        expected=((), (), (("pc", builder.CODE_BASE + 8), ("sve_live", 1)),
                  builder.NO_SME, (((0, p(0xFFFF)),), p(0x0001))),
        memory=(), ordinals=(0, 0))
    options = 1 << builder.OPTION_NAMES.index("without_sve")
    record = builder.Record(
        name="synthetic/sve", producer="synthetic", site="synthetic:2",
        options=options, profiles=3, vl=builder.SVE_VL,
        programs=[(0, live.words)], cases=[(0, live), (0, becomes_live)])
    synthetic = builder.Corpus(meta={"producer_commit": "-"}, blobs={},
                               records=[record])
    tokens = builder.feature_tokens()
    encoded = builder.encode_pack(synthetic, tokens)
    decoded, decoded_tokens = builder.decode_pack(encoded)
    if decoded.records != synthetic.records or \
            builder.encode_pack(decoded, decoded_tokens) != encoded:
        raise SystemExit("an SVE record does not survive the pack")
    reloaded, reloaded_tokens = builder.load_dump(
        builder.dump_corpus(synthetic, tokens))
    if reloaded.records != synthetic.records or \
            builder.encode_pack(reloaded, reloaded_tokens) != encoded:
        raise SystemExit("an SVE record does not survive the text form")
    try:
        builder.encode_pack(synthetic, tokens, 2)
    except SystemExit:
        pass
    else:
        raise SystemExit("a version 2 pack accepts a VL")
    broken = {
        "P while the state is not live": dict(initial=(
            (), (), (), builder.NO_SME, (((1, p(1)),), None))),
        "FFR while the state is not live": dict(initial=(
            (), (), (), builder.NO_SME, ((), p(1)))),
        "P left behind when the state stops being live": dict(expected=(
            (), (), (("sve_live", 0),), builder.NO_SME, builder.NO_SVE)),
        "short P": dict(initial=(
            (), (), (("sve_live", 1),), builder.NO_SME,
            (((1, b"\x01"),), None))),
        "P16": dict(initial=(
            (), (), (("sve_live", 1),), builder.NO_SME,
            (((16, p(1)),), None))),
    }
    for reason, change in broken.items():
        case = builder.dataclasses.replace(live, **change)
        if builder.invalid_sve_state(record, case) is None:
            raise SystemExit(f"the encoder accepts {reason}")
    for reason, changed in (
            ("SVE state without a VL",
             builder.dataclasses.replace(record, vl=0)),
            ("a VL other than 16",
             builder.dataclasses.replace(record, vl=32))):
        if builder.invalid_sve_state(changed, live) is None:
            raise SystemExit(f"the encoder accepts {reason}")


def version_2_round_trip(builder, corpus, tokens, pack: bytes) -> None:
    """Every record of the pack keeps its meaning, bit for bit, in a version
    2 pack, which has no VLs: decoding that pack gives the same records, and
    they re-encode to the version 3 pack."""
    if any(record.vl for record in corpus.records):
        corpus = builder.Corpus(
            meta=corpus.meta, blobs=corpus.blobs,
            records=[record for record in corpus.records if not record.vl])
        pack = builder.encode_pack(corpus, tokens)
    version_2 = builder.encode_pack(corpus, tokens, 2)
    decoded, decoded_tokens = builder.decode_pack(version_2)
    if decoded.records != corpus.records or decoded_tokens != tokens or \
            builder.encode_pack(decoded, decoded_tokens) != pack:
        raise SystemExit("the pack's records do not survive pack version 2")


def main() -> int:
    builder = load_builder()
    sme_round_trip(builder)
    sve_round_trip(builder)
    # Metadata and the canonical encoding must match the generated pack.
    pack_path = pathlib.Path(sys.argv[1])
    builder.check(argparse.Namespace(output=pack_path.parent))

    # The reviewable text form must reproduce the binary form exactly.
    pack = pack_path.read_bytes()
    corpus, tokens = builder.decode_pack(pack)
    version_2_round_trip(builder, corpus, tokens, pack)

    # The pack carries the LSE2 alignment rewrite of the current script.
    step = corpus.meta.get("lse2_alignment")
    if step is None or hashlib.sha256(
            (REPOSITORY / step["script"]).read_bytes()).hexdigest() != \
            step["sha256"]:
        raise SystemExit("corpus lacks the current LSE2 alignment rewrite")
    step = max(1, len(corpus.records) // ROUND_TRIP_RECORDS)
    records = corpus.records[::step]
    used = {region.blob for record in records for _program, case in record.cases
            for region in case.memory}
    subset = builder.Corpus(
        meta=corpus.meta,
        blobs={digest: data for digest, data in corpus.blobs.items()
               if digest in used},
        records=records,
    )
    encoded = builder.encode_pack(subset, tokens)
    reloaded, reloaded_tokens = builder.load_dump(
        builder.dump_corpus(subset, tokens))
    if builder.encode_pack(reloaded, reloaded_tokens) != encoded:
        raise SystemExit("corpus text dump does not re-pack to the same bytes")
    decoded, _decoded_tokens = builder.decode_pack(encoded)
    if [record.name for record in decoded.records] != \
            [record.name for record in records]:
        raise SystemExit("corpus subset does not decode to its records")
    print(f"A64 instruction corpus round trip: {len(records)} records")
    return 0


if __name__ == "__main__":
    sys.exit(main())
