#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reject a missing, split-generation, or noncanonical A64 ASLRef corpus."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import sys
from pathlib import Path

from a64_aslref_corpus_identity import validate_corpus_trio
from generate_a64_aslref_corpus import (
    COLUMNS,
    QUEUE_COLUMNS,
    validate_corpus_rows,
    validate_queue_rows,
)


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--corpus",
        type=Path,
        default=repository / "tests/unit/arm64/a64_aslref_allocation_corpus.tsv",
    )
    parser.add_argument(
        "--interface-queue",
        type=Path,
        default=repository / "tests/unit/arm64/a64_aslref_interface_queue.tsv",
    )
    parser.add_argument(
        "--identity",
        type=Path,
        default=repository / "tests/unit/arm64/a64_aslref_corpus.identity.json",
    )
    return parser.parse_args()


def main() -> int:
    options = arguments()
    validate_corpus_trio(
        options.corpus,
        COLUMNS,
        options.interface_queue,
        QUEUE_COLUMNS,
        options.identity,
        corpus_validator=validate_corpus_rows,
        queue_validator=validate_queue_rows,
    )
    print(
        "A64 ASLRef corpus generation identity passed: "
        f"{options.corpus}, {options.interface_queue}, {options.identity}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
