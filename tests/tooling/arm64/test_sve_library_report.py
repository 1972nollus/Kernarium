#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check tools/arm64-tests/sve-library-report.py on reports it must sum.

The gadget engine writes one line per encoding-table row; the tool names each
row from the checked-in table, ranks the rows by exits, and refuses a report
whose mask and value are another table's.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import pathlib
import subprocess
import sys
import tempfile

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
TOOLS = REPOSITORY / "tools" / "arm64-tests"
sys.path.insert(0, str(TOOLS))
import sme_encoding_table  # noqa: E402


def line(row, counts) -> str:
    return (f"SVE-{row.form:04d}\t{row.index}\t{row.mask:08x}\t{row.value:08x}"
            f"\t{counts[0]}\t{counts[1]}\t{counts[2]}")


def run(*arguments) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-B", str(TOOLS / "sve-library-report.py"),
         *map(str, arguments)], capture_output=True, text=True, check=False)


def main() -> int:
    table = sme_encoding_table.load(sme_encoding_table.TABLE)
    named = {}
    for row in table.rows:
        if row.encoding in ("fexpa_z_z_", "ftmad_z_zzi_", "fscale_z_p_zz_"):
            named.setdefault(row.encoding, row)
    if len(named) != 3:
        print("the encoding table lacks FEXPA, FTMAD or FSCALE")
        return 1
    fexpa_rows = [row for row in table.rows if row.encoding == "fexpa_z_z_"]
    if len(fexpa_rows) < 2:
        print("the encoding table has one FEXPA row")
        return 1
    with tempfile.TemporaryDirectory() as directory:
        first = pathlib.Path(directory) / "first.tsv"
        second = pathlib.Path(directory) / "second.tsv"
        first.write_text("# SVE library exits: total 40\n# header\n" + "\n".join([
            line(named["fexpa_z_z_"], (3, 3, 30)),
            line(named["fscale_z_p_zz_"], (1, 1, 10)),
        ]) + "\n")
        second.write_text("# SVE library exits: total 60\n" + "\n".join([
            line(named["ftmad_z_zzi_"], (2, 2, 50)),
            line(fexpa_rows[1], (1, 1, 10)),
        ]) + "\n")
        result = run(first, second)
        if result.returncode != 0:
            print(result.stdout + result.stderr)
            return 1
        lines = result.stdout.splitlines()
        # The total, a header, then the rows by exits: FTMAD 50, FEXPA 40,
        # FSCALE 10 (shares of 100).
        expected = ["ftmad_z_zzi_", "fexpa_z_z_", "fscale_z_p_zz_"]
        got = [row.split()[5] for row in lines[2:]]
        if lines[0] != "SVE words through the library: 100" or got != expected:
            print(result.stdout)
            return 1
        if "40.00%" not in lines[3] or "(2 rows)" not in lines[3]:
            print(result.stdout)
            return 1
        by_row = run(first, second, "--rows")
        rows_first = by_row.stdout.splitlines()
        if by_row.returncode != 0 or "(2 rows)" in by_row.stdout or \
                len(rows_first) != 6:
            print(by_row.stdout + by_row.stderr)
            return 1
        bad = pathlib.Path(directory) / "bad.tsv"
        row = named["fexpa_z_z_"]
        bad.write_text(f"SVE-{row.form:04d}\t{row.index}\t{row.mask:08x}\t"
                       f"{row.value ^ 1:08x}\t1\t1\t1\n")
        if run(bad).returncode == 0:
            print("a report of another table was accepted")
            return 1
    print("sve-library-report ranks and sums reports and refuses another table")
    return 0


if __name__ == "__main__":
    sys.exit(main())
