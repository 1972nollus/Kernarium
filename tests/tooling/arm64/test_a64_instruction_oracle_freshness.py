#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate both A64 oracle tables and require the checked-in bytes.

The pinned Arm XML archive is not a managed dependency, so this check is
opt-in: CMake passes UML_A64_ARCHITECTURE_ARCHIVE, and the test skips when
that is empty or missing, or when the pinned LLVM MC is not installed. Both
generators refuse an archive whose name or SHA-256 differs from the pin in
a64_architecture_oracle.py before they read it.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from a64_llvm_oracle import PINNED_LLVM_VERSION, find_llvm_mc, llvm_version


SKIP = 77
TOOLING = Path(__file__).resolve().parent
TABLES = Path(__file__).resolve().parents[3] / "tests/unit/arm64"
GENERATORS = {
    "a64_instruction_allocations.tsv": "generate_a64_instruction_allocations.py",
    "a64_instruction_decode_partitions.tsv": (
        "generate_a64_instruction_decode_partitions.py"
    ),
}
GENERATOR_TIMEOUT_SECONDS = 600


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--architecture-archive", default="")
    options = parser.parse_args()
    if not options.architecture_archive:
        print("skipped: UML_A64_ARCHITECTURE_ARCHIVE is not set")
        return SKIP
    archive = Path(options.architecture_archive)
    if not archive.is_file():
        print(f"skipped: {archive} does not exist")
        return SKIP
    try:
        tool = find_llvm_mc().resolve(strict=True)
        version = llvm_version(tool)
    except (OSError, ValueError, subprocess.SubprocessError):
        version = None
    if version != PINNED_LLVM_VERSION:
        print(f"skipped: LLVM MC {PINNED_LLVM_VERSION} is not installed")
        return SKIP

    stale: list[str] = []
    with tempfile.TemporaryDirectory() as scratch:
        output = Path(scratch).resolve(strict=True)
        for table, generator in GENERATORS.items():
            command = [
                sys.executable,
                "-B",
                str(TOOLING / generator),
                "--architecture-archive",
                str(archive.resolve(strict=True)),
                "--output",
                str(output / table),
            ]
            if table == "a64_instruction_allocations.tsv":
                command += ["--llvm-mc", str(tool)]
            subprocess.run(
                command,
                check=True,
                stdout=subprocess.DEVNULL,
                timeout=GENERATOR_TIMEOUT_SECONDS,
            )
            if (output / table).read_bytes() != (TABLES / table).read_bytes():
                stale.append(table)
    if stale:
        print(
            "A64 oracle tables differ from a regeneration: "
            + ", ".join(stale)
            + "; rerun their generators",
            file=sys.stderr,
        )
        return 1
    print("A64 oracle tables match a regeneration from the pinned release")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
