#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate the SVE/SME encoding table and require the checked-in bytes.

Like arm64-instruction-oracle-freshness, this check is opt-in: CMake passes
UML_A64_ARCHITECTURE_ARCHIVE, and the test skips when that is empty or
missing, or when the pinned LLVM MC is not installed. The generator refuses
an archive whose name or SHA-256 differs from the pin before it reads it.
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
TABLE = Path(__file__).resolve().parents[3] / "darwin/cpu/arm64/detail/sme_encodings.hpp"
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

    with tempfile.TemporaryDirectory() as scratch:
        output = Path(scratch).resolve(strict=True) / TABLE.name
        subprocess.run(
            [
                sys.executable,
                "-B",
                str(TOOLING / "generate_sme_encoding_table.py"),
                "--architecture-archive",
                str(archive.resolve(strict=True)),
                "--llvm-mc",
                str(tool),
                "--output",
                str(output),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            timeout=GENERATOR_TIMEOUT_SECONDS,
        )
        if output.read_bytes() != TABLE.read_bytes():
            print(
                f"{TABLE.name} differs from a regeneration; rerun "
                "generate_sme_encoding_table.py",
                file=sys.stderr,
            )
            return 1
    print("SVE/SME encoding table matches a regeneration from the pinned release")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
