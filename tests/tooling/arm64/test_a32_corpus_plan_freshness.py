#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Plan every AArch32 corpus suite again and require the checked-in tables.

The corpus planner draws words per form and starts recipes and FPSCR values
by case ordinal, so a change to the forms it iterates over (a corrected
name, a new form) moves cases and leaves a source table that its planner no
longer reproduces. This check runs `a32-corpus.py check`, which plans each
suite without an oracle and compares name, form, program, recipe and starting
state with the table's rows, in order.

Like arm64-a32-encoding-table-freshness, it is opt-in: CMake passes
UML_A32_ARCHITECTURE_ARCHIVE, and the test skips when that is empty or
missing. It needs neither LLVM MC nor a host that runs AArch32 programs.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import subprocess
import sys
from pathlib import Path


SKIP = 77
REPOSITORY = Path(__file__).resolve().parents[3]
TIMEOUT_SECONDS = 600


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--architecture-archive", default="")
    options = parser.parse_args()
    if not options.architecture_archive:
        print("skipped: UML_A32_ARCHITECTURE_ARCHIVE is not set")
        return SKIP
    archive = Path(options.architecture_archive)
    if not archive.is_file():
        print(f"skipped: {archive} does not exist")
        return SKIP
    result = subprocess.run(
        [
            sys.executable,
            "-B",
            str(REPOSITORY / "tools/arm64-tests/a32-corpus.py"),
            "check",
            "--archive",
            str(archive.resolve(strict=True)),
        ],
        stdin=subprocess.DEVNULL,
        timeout=TIMEOUT_SECONDS,
    )
    return 1 if result.returncode else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
