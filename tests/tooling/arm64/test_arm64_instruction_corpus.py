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


def main() -> int:
    builder = load_builder()
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
