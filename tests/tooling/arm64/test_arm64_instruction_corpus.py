#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the A64 instruction corpus metadata and its text round trip."""

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
                  ((0, z(1)), (63, z(2))), z(3)[:builder.ZT0_BYTES])),
        expected=(((1, 7),), (), (("pc", builder.CODE_BASE + 8),),
                  (((5, z(5)),), (), ((17, z(4)),), None)),
        memory=(), ordinals=(0, 0))
    za_only = builder.Case(
        name="za only", entry=0, words=((0, 0xC080_0000), (4, 0xD400_0001)),
        event="illegal-instruction", invalid_encoding=True,
        minimization="full", retired=("exact", 0), fault=None, syndrome=0,
        native=("sme",), validator=None,
        initial=((), ((2, bytes(16)),), (("svcr", 2),),
                 ((), (), ((5, z(6)),), None)),
        expected=((), (), (), builder.NO_SME), memory=(), ordinals=(0, 0))
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
            (), ((0, bytes(16)),), (("svcr", 1),), builder.NO_SME)),
        "Z outside streaming mode": dict(initial=(
            (), (), (("svcr", 2),), (((0, z(0)),), (), (), None))),
        "ZT0 while PSTATE.ZA is clear": dict(initial=(
            (), (), (("svcr", 1),), ((), (), (), z(0)[:64]))),
        "short Z": dict(initial=(
            (), (), (("svcr", 1),), (((0, z(0)[:32]),), (), (), None))),
    }
    for reason, change in broken.items():
        case = builder.dataclasses.replace(streaming, **change)
        if builder.invalid_sme_state(record, case) is None:
            raise SystemExit(f"the encoder accepts {reason}")
    plain = builder.dataclasses.replace(record, svl=0)
    if builder.invalid_sme_state(plain, streaming) is None:
        raise SystemExit("the encoder accepts SME state without an SVL")


def main() -> int:
    builder = load_builder()
    sme_round_trip(builder)
    # Metadata and the canonical encoding must match the checked-in pack.
    builder.check(argparse.Namespace(output=builder.CORPUS))

    # The reviewable text form must reproduce the binary form exactly.
    pack = (builder.CORPUS / builder.PACK_NAME).read_bytes()
    corpus, tokens = builder.decode_pack(pack)

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
