#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarize execution-weighted scalar and SIMD/FP forwarding opportunities.

Counters are exact per entered decoder group. Time-window attribution uses
100-ms snapshots and host-observed console markers, not retired instructions.
Diagnostic runs are not performance scores. No workloads are averaged equally:
the summary divides totals of entered groups and declared source accessors.
Conditional accessors are potential reads; this does not infer taken CCMP
conditions, retirement after a fault, or data dependencies across block exits.
"""
from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import argparse
import csv
import json
from pathlib import Path
import re

LEGACY_BUCKET = re.compile(r"^(add_imm|sub_imm|add_reg|sub_reg|and|orr|eor|ands)_(w|x)_f([01])_m([0-3])$")
BUCKET = re.compile(r"^([a-z_]+)_s([0-3])_m([0-7])$")
METRICS = ("groups", "forwarded_groups", "source_reads", "forwarded_reads")


def read_counts(path: Path) -> list[dict[str, int]]:
    with path.open() as source:
        reader = csv.DictReader(source)
        if not reader.fieldnames or reader.fieldnames[:3] != [
            "monotonic_ns", "entered_guest_words", "unsupported_groups"
        ]:
            raise ValueError("unexpected dependency counter schema")
        buckets = reader.fieldnames[3:]
        legacy = len(buckets) == 128 and all(LEGACY_BUCKET.fullmatch(name) for name in buckets)
        families = list(dict.fromkeys(
            match[1] for name in buckets if (match := BUCKET.fullmatch(name))))
        expected = [f"{family}_s{sources}_m{mask}" for family in families
                    for sources in range(4) for mask in range(8)]
        if not legacy and (not families or buckets != expected):
            raise ValueError("unexpected scalar/SIMD dependency buckets")
        rows = []
        for row in reader:
            if None in row or any(value is None for value in row.values()):
                raise ValueError("incomplete dependency counter snapshot")
            values = {key: int(value) for key, value in row.items()}
            if any(value < 0 for value in values.values()):
                raise ValueError("negative dependency counter")
            rows.append(values)
    if len(rows) < 2:
        raise ValueError("need at least two complete counter snapshots")
    for before, after in zip(rows, rows[1:]):
        if any(after[key] < before[key] for key in after):
            raise ValueError("counter snapshot moved backwards")
    return rows


def aggregate(families: dict[str, dict[str, int]], words: int) -> dict[str, object]:
    totals = {key: sum(row[key] for row in families.values()) for key in METRICS}
    def percent(numerator: int, denominator: int) -> float:
        return 100 * numerator / denominator if denominator else 0.0
    return {
        "eligible_groups": totals["groups"],
        "forwardable_groups": totals["forwarded_groups"],
        "eligible_source_reads": totals["source_reads"],
        "forwardable_source_reads": totals["forwarded_reads"],
        "eligible_word_percent": percent(totals["groups"], words),
        "forwardable_word_percent": percent(totals["forwarded_groups"], words),
        "forwardable_eligible_percent": percent(totals["forwarded_groups"], totals["groups"]),
        "forwardable_source_percent": percent(totals["forwarded_reads"], totals["source_reads"]),
        "families": families,
    }


def summarize(values: dict[str, int]) -> dict[str, object]:
    families: dict[str, dict[str, int]] = {}
    for key, value in values.items():
        match = BUCKET.fullmatch(key)
        if match:
            family, source_text, mask_text = match.groups()
            source_count = int(source_text)
        elif (match := LEGACY_BUCKET.fullmatch(key)):
            family, _, _, mask_text = match.groups()
            source_count = 1 if family.endswith("_imm") else 2
        else:
            continue
        mask = int(mask_text)
        if value and mask.bit_count() > source_count:
            raise ValueError("forwarding mask exceeds the declared source count")
        row = families.setdefault(family, dict.fromkeys(METRICS, 0))
        row["groups"] += value
        row["forwarded_groups"] += value * bool(mask)
        row["source_reads"] += value * source_count
        row["forwarded_reads"] += value * mask.bit_count()
    words = values["entered_guest_words"]
    combined = aggregate(families, words)
    scalar = {name: row for name, row in families.items() if name != "simd"}
    vectors = {name: row for name, row in families.items() if name == "simd"}
    return {
        "entered_guest_words": words,
        "entered_decode_groups": combined["eligible_groups"] + values["unsupported_groups"],
        **combined,
        "domains": {
            "scalar": aggregate(scalar, words),
            # Old recordings did not observe vector dependencies: unavailable
            # is distinct from an observed zero, and is never inferred as zero.
            "simd_fp": aggregate(vectors, words) if vectors else None,
        },
    }


def interval(rows: list[dict[str, int]], start: int, end: int) -> dict[str, object]:
    if end <= start:
        raise ValueError("empty dependency window")
    before = min(rows, key=lambda row: abs(row["monotonic_ns"] - start))
    after = min(rows, key=lambda row: abs(row["monotonic_ns"] - end))
    if after["monotonic_ns"] <= before["monotonic_ns"]:
        raise ValueError("dependency window is shorter than snapshot resolution")
    values = {key: after[key] - before[key] for key in before if key != "monotonic_ns"}
    return {
        "requested_start_ns": start, "requested_end_ns": end,
        "snapshot_start_ns": before["monotonic_ns"],
        "snapshot_end_ns": after["monotonic_ns"],
        **summarize(values),
    }


def geekbench_windows(rows: list[dict[str, int]], events: list[dict[str, str]]) -> dict[str, object]:
    phases = ("Single-Core", "Multi-Core")
    starts = [event for event in events if event["phase"] in phases
              and event["workload"] == "between-workloads"]
    ends = [event for event in events if event["phase"] == "complete"]
    order = [event["phase"] for event in starts]
    if order not in (["Single-Core"], ["Single-Core", "Multi-Core"]) or len(ends) != 1:
        raise ValueError("requires a completed single-core or single/multi-core Geekbench run")
    times = [int(event["monotonic_ns"]) for event in events]
    if times != sorted(times):
        raise ValueError("Geekbench events moved backwards")
    end = int(ends[0]["monotonic_ns"])
    sections, all_workloads = [], []
    for index, event in enumerate(starts):
        phase, start = event["phase"], int(event["monotonic_ns"])
        stop = int(starts[index + 1]["monotonic_ns"]) if index + 1 < len(starts) else end
        completed = [item for item in events if item["phase"] == f"{phase}-completed"]
        if not completed:
            raise ValueError(f"missing {phase} workload completion events")
        workloads, previous = [], start
        for item in completed:
            boundary = int(item["monotonic_ns"])
            if not start < boundary <= stop:
                raise ValueError("workload completion is outside its core-mode section")
            workloads.append({"phase": phase, "workload": item["workload"],
                              **interval(rows, previous, boundary)})
            previous = boundary
        sections.append({"phase": phase, "summary": interval(rows, start, stop),
                         "completed_workloads": workloads})
        all_workloads.extend(workloads)
    return {"summary": interval(rows, int(starts[0]["monotonic_ns"]), end),
            "sections": sections, "completed_workloads": all_workloads}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("counters", type=Path)
    windows = parser.add_mutually_exclusive_group(required=True)
    windows.add_argument("--geekbench-events", type=Path)
    windows.add_argument("--aquarium-metrics", type=Path)
    args = parser.parse_args()
    rows = read_counts(args.counters)
    version = (3 if "simd_s0_m0" in rows[0] else
               2 if any(BUCKET.fullmatch(key) for key in rows[0]) else 1)
    result: dict[str, object] = {"schema_version": version, "counter_interval_ms": 100}
    if args.geekbench_events:
        with args.geekbench_events.open() as source:
            events = list(csv.DictReader(source, delimiter="\t"))
        result.update(geekbench_windows(rows, events))
        result["console_poll_interval_ms"] = 100
    else:
        metrics = json.loads(args.aquarium_metrics.read_text())
        samples = metrics["host_measurement"]["snapshots"]
        start = round(samples[0]["monotonic_seconds"] * 1e9)
        end = round(samples[-1]["monotonic_seconds"] * 1e9)
        result["summary"] = interval(rows, start, end)
        result["console_poll_interval_ms"] = 500
        result["fish"] = metrics["FISH"]
        result["sample_count"] = metrics["SAMPLES"]
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
