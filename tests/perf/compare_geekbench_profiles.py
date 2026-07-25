#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare completed workload telemetry from two Geekbench profile runs."""

from __future__ import annotations

import argparse
from pathlib import Path

from analyze_geekbench_profile import read_telemetry, workload_ranges


def completed(path: Path) -> dict[tuple[str, str], tuple[float, float]]:
    groups = workload_ranges(read_telemetry(path / "host-cpu.csv"))
    result: dict[tuple[str, str], tuple[float, float]] = {}
    # The active tail has no following transition and is therefore incomplete.
    for group in groups[:-1]:
        if len(group) < 2 or group[0].workload in {
            "startup", "between-workloads", "upload", "complete"
        }:
            continue
        wall = group[-1].epoch - group[0].epoch
        cpu = group[-1].cpu_seconds - group[0].cpu_seconds
        result[(group[0].phase, group[0].workload)] = (wall, cpu / wall)
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    args = parser.parse_args()

    baseline = completed(args.baseline)
    candidate = completed(args.candidate)
    print(
        "phase\tworkload\tbaseline_s\tcandidate_s\twall_delta_pct\t"
        "baseline_cores\tcandidate_cores")
    for key, (baseline_wall, baseline_cores) in baseline.items():
        if key not in candidate:
            continue
        candidate_wall, candidate_cores = candidate[key]
        delta = (candidate_wall / baseline_wall - 1.0) * 100.0
        print(
            f"{key[0]}\t{key[1]}\t{baseline_wall:.3f}\t"
            f"{candidate_wall:.3f}\t{delta:+.2f}\t"
            f"{baseline_cores:.3f}\t{candidate_cores:.3f}")


if __name__ == "__main__":
    main()
