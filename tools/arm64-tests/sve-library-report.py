#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Name and rank the SVE library-exit report of a gadget engine run.

Setting DARWIN_UML_SVE_LIBRARY_EXITS=PATH makes a process that links the
gadget engine count, per encoding-table row, the SVE words the builder
translated, those it sent to the semantic library for want of a generated
gadget, and the executions of words that took the library (darwin/cpu/arm64/
gadgets/sve_helpers.hpp). It writes PATH at exit. This tool reads one or more
such reports, sums them, and prints the forms (the rows of one encoding
together; --rows keeps them apart) by exits with their names and share of
all exits, so that a form is lowered by the weight real code gives it.

usage: sve-library-report.py [--top N] [--rows] REPORT [REPORT ...]
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sme_encoding_table  # noqa: E402


def read_report(path: pathlib.Path) -> dict[int, list[int]]:
    """Row -> [translated, translated_to_library, exits, mask, value], the
    last two kept to catch a report from another table."""
    rows: dict[int, list[int]] = {}
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 7 or not fields[0].startswith("SVE-"):
            raise ValueError(f"{path}:{number}: not a report line")
        row = int(fields[1])
        counts = [int(fields[4]), int(fields[5]), int(fields[6])]
        previous = rows.get(row, [0, 0, 0])
        rows[row] = [a + b for a, b in zip(previous, counts)] + [
            int(fields[2], 16), int(fields[3], 16)]
    return rows


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--top", type=int, default=40)
    parser.add_argument("--rows", action="store_true",
                        help="rank table rows, not encodings")
    parser.add_argument("reports", nargs="+", type=pathlib.Path)
    arguments = parser.parse_args(argv)
    table = sme_encoding_table.load(sme_encoding_table.TABLE)
    by_index = {row.index: row for row in table.rows}
    totals: dict[int, list[int]] = {}
    for path in arguments.reports:
        for row, counts in read_report(path).items():
            current = totals.setdefault(row, [0, 0, 0, counts[3], counts[4]])
            for k in range(3):
                current[k] += counts[k]
            if current[3:] != counts[3:]:
                raise ValueError(f"{path}: row {row} has another encoding "
                                 "than the other reports (different table)")
    total_exits = sum(counts[2] for counts in totals.values())
    groups: dict[tuple, list] = {}
    for row, counts in totals.items():
        entry = by_index.get(row)
        if entry is None or (entry.mask, entry.value) != tuple(counts[3:]):
            raise ValueError(f"row {row}: the encoding table differs from the "
                             "report's")
        key = (row,) if arguments.rows else (entry.form, entry.encoding)
        group = groups.setdefault(key, [0, 0, 0, entry, 0])
        for k in range(3):
            group[k] += counts[k]
        group[4] += 1
    print(f"SVE words through the library: {total_exits}")
    print(f"{'exits':>12} {'share':>7} {'translated':>11} {'to library':>11}"
          "  form  name")
    ranked = sorted(groups.items(), key=lambda item: (
        -item[1][2], -item[1][0], item[0]))
    for _key, group in ranked[:arguments.top]:
        entry = group[3]
        share = 100.0 * group[2] / total_exits if total_exits else 0.0
        rows = "" if arguments.rows or group[4] == 1 else f" ({group[4]} rows)"
        print(f"{group[2]:12d} {share:6.2f}% {group[0]:11d} {group[1]:11d}"
              f"  SVE-{entry.form:04d}  {entry.encoding}{rows}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
