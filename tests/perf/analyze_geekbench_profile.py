#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarize profile_geekbench.py telemetry and macOS sample output."""

from __future__ import annotations

import argparse
import csv
import re
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


TOP_ENTRY = re.compile(r"^\s*(.+?)\s{2,}(\d+)\s*$")


@dataclass(frozen=True)
class Telemetry:
    epoch: float
    cpu_seconds: float
    cpu_percent: float
    phase: str
    workload: str


def cpu_seconds(value: str) -> float:
    fields = value.split(":")
    if len(fields) == 2:
        minutes, seconds = fields
        return int(minutes) * 60 + float(seconds)
    if len(fields) == 3:
        hours, minutes, seconds = fields
        return int(hours) * 3600 + int(minutes) * 60 + float(seconds)
    raise ValueError(f"unrecognized process CPU time: {value!r}")


def read_telemetry(path: Path) -> list[Telemetry]:
    with path.open(newline="", encoding="utf-8") as source:
        return [
            Telemetry(
                epoch=float(row["epoch"]),
                cpu_seconds=cpu_seconds(row["cpu_time"]),
                cpu_percent=float(row["cpu_percent"]),
                phase=row["phase"],
                workload=row["workload"],
            )
            for row in csv.DictReader(source)
        ]


def workload_ranges(rows: list[Telemetry]) -> list[list[Telemetry]]:
    result: list[list[Telemetry]] = []
    for row in rows:
        key = (row.phase, row.workload)
        if not result or (result[-1][0].phase, result[-1][0].workload) != key:
            result.append([row])
        else:
            result[-1].append(row)
    return result


def sample_top_stacks(path: Path) -> Counter[str]:
    result: Counter[str] = Counter()
    in_top = False
    with path.open(encoding="utf-8", errors="replace") as source:
        for line in source:
            if line.startswith("Sort by top of stack"):
                in_top = True
                continue
            if in_top and line.startswith("Binary Images:"):
                break
            if not in_top:
                continue
            match = TOP_ENTRY.match(line)
            if match:
                result[match.group(1)] += int(match.group(2))
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("profile_dir", type=Path)
    parser.add_argument("--top", type=int, default=30)
    args = parser.parse_args()

    rows = read_telemetry(args.profile_dir / "host-cpu.csv")
    groups = workload_ranges(rows)
    print("status\tphase\tworkload\twall_s\tcpu_s\thost_cores\tmean_ps_cpu")
    for index, group in enumerate(groups):
        if len(group) < 2:
            continue
        wall = group[-1].epoch - group[0].epoch
        consumed = group[-1].cpu_seconds - group[0].cpu_seconds
        mean_cpu = sum(row.cpu_percent for row in group) / len(group)
        status = "partial" if index == len(groups) - 1 else "complete"
        print(
            f"{status}\t{group[0].phase}\t{group[0].workload}\t{wall:.3f}\t"
            f"{consumed:.3f}\t{consumed / wall:.3f}\t{mean_cpu:.1f}")

    aggregate: Counter[str] = Counter()
    for sample_path in sorted(args.profile_dir.glob("*.sample.txt")):
        aggregate.update(sample_top_stacks(sample_path))
    print("\ncount\tsymbol")
    shown = 0
    for symbol, count in aggregate.most_common():
        if "(in uml-darwin)" not in symbol:
            continue
        print(f"{count}\t{symbol}")
        shown += 1
        if shown == args.top:
            break


if __name__ == "__main__":
    main()
