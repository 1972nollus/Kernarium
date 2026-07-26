#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a per-workload hotspot matrix from a Geekbench profile directory."""

from __future__ import annotations

import argparse
import csv
from collections import Counter
from pathlib import Path

from analyze_geekbench_profile import sample_top_stacks


CATEGORIES = (
    "memory",
    "control",
    "integer",
    "fp_simd",
    "runtime",
    "other",
)


def symbol_name(entry: str) -> str:
    return entry.split("  (in ", 1)[0]


def category(entry: str) -> str:
    name = symbol_name(entry).lower()
    if not name.startswith("uml_arm64_gadget_"):
        return "runtime"
    if any(token in name for token in (
        "load", "store", "prefetch", "memory", "exclusive", "atomic",
    )):
        return "memory"
    if any(token in name for token in (
        "branch", "compare_branch", "test_branch", "conditional_select",
    )):
        return "control"
    if any(token in name for token in (
        "simd", "floating", "vector", "scalar_fp", "bfloat", "half_",
        "fp8", "fcvt", "fmov", "fused_",
    )):
        return "fp_simd"
    if any(token in name for token in (
        "add_sub", "logical", "bitfield", "multiply", "divide",
        "variable_shift", "bit_reverse", "crc", "extract",
    )):
        return "integer"
    return "other"


def sample_events(profile_dir: Path) -> list[tuple[str, str, Path]]:
    result: list[tuple[str, str, Path]] = []
    with (profile_dir / "workloads.tsv").open(
        newline="", encoding="utf-8"
    ) as source:
        for row in csv.DictReader(source, delimiter="\t"):
            sample = row["sample"]
            if not sample or sample == "skipped-previous-sample-active":
                continue
            path = profile_dir / sample
            if path.is_file():
                result.append((row["phase"], row["workload"], path))
    return result


def emulator_stacks(path: Path) -> Counter[str]:
    return Counter({
        symbol: count
        for symbol, count in sample_top_stacks(path).items()
        if "(in uml-darwin" in symbol
    })


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("profile_dir", type=Path)
    parser.add_argument("--top", type=int, default=8)
    parser.add_argument(
        "--phase",
        choices=("Single-Core", "Multi-Core"),
        help="limit output to one Geekbench phase",
    )
    args = parser.parse_args()
    if args.top < 1:
        raise SystemExit("--top must be positive")

    profiles: list[tuple[str, str, Counter[str]]] = []
    for phase, workload, path in sample_events(args.profile_dir):
        if args.phase is None or phase == args.phase:
            profiles.append((phase, workload, emulator_stacks(path)))

    print(
        "phase\tworkload\temulator_samples\tmemory_pct\tcontrol_pct\t"
        "integer_pct\tfp_simd_pct\truntime_pct\tother_pct\ttop_symbol"
    )
    for phase, workload, stacks in profiles:
        total = stacks.total()
        categories = Counter({
            name: sum(
                count for symbol, count in stacks.items()
                if category(symbol) == name
            )
            for name in CATEGORIES
        })
        shares = [
            100.0 * categories[name] / total if total else 0.0
            for name in CATEGORIES
        ]
        top_symbol = symbol_name(stacks.most_common(1)[0][0]) if stacks else ""
        print(
            f"{phase}\t{workload}\t{total}\t"
            + "\t".join(f"{share:.1f}" for share in shares)
            + f"\t{top_symbol}"
        )

    print("\nphase\tworkload\trank\tcount\tshare_pct\tcategory\tsymbol")
    for phase, workload, stacks in profiles:
        total = stacks.total()
        for rank, (symbol, count) in enumerate(
            stacks.most_common(args.top), start=1
        ):
            share = 100.0 * count / total if total else 0.0
            print(
                f"{phase}\t{workload}\t{rank}\t{count}\t{share:.1f}\t"
                f"{category(symbol)}\t{symbol_name(symbol)}"
            )


if __name__ == "__main__":
    main()
